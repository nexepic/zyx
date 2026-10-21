#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyAllocationAdmissionTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    Request admission(const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto result = request(NODE, memory); result.conversion->publish_candidate = true;
        result.conversion->allocation_admission.emplace();
        result.conversion->allocation_admission->maximum_admitted_peak_bytes = 32U * 1024U * 1024U;
        result.conversion->allocation_admission->maximum_admitted_file_bytes = 32U * 1024U * 1024U; return result;
    }
    NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions result; result.first_slot = PhysicalSlotId(40); result.last_slot = PhysicalSlotId(80);
        result.entity_pool_owner_id = 901; result.logical_id_lease_block_size = 1;
        result.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902}; return result;
    }
    void replaceRow(uint64_t id, PropertyValue value, PropertyCollectionRootStore &roots) {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, placement());
        auto tx = planner.beginTransaction(); tx.replaceNodeProperty({id, graph::Node::typeId, {{1, std::move(value)}}});
        ASSERT_TRUE(tx.commit().committed());
    }
    struct Stats {
        int64_t row = 0, candidate = 0, peak = 0, prepared = 0, insufficient = 0, unknown = 0, resource = 0;
        static void observe(void *owner, std::string_view key, int64_t value, bool) noexcept {
            auto &stats = *static_cast<Stats *>(owner);
            if (key == "native_graph.hybrid.allocation_row_bytes") stats.row = value;
            if (key == "native_graph.hybrid.allocation_candidate_bytes") stats.candidate = value;
            if (key == "native_graph.hybrid.allocation_peak_bytes") stats.peak = value;
            if (key == "native_graph.hybrid.allocation_costs_prepared") stats.prepared += value;
            if (key == "native_graph.hybrid.allocation_admission_insufficient_saving") stats.insufficient += value;
            if (key == "native_graph.hybrid.allocation_admission_unknown") stats.unknown += value;
            if (key == "native_graph.hybrid.allocation_admission_resource_refusal") stats.resource += value;
        }
    };
};
}
TEST_F(HybridPropertyAllocationAdmissionTest, DefaultFormatStillRefusesTheIsolatedConversionBeforeIO) {
#if !ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW(run(admission(memory)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
#endif
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyAllocationAdmissionTest, CompletePreparedCostsRejectTheSmallDenseShapeAndKeepItsRowAuthority) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto expected = properties_->readEntity(old, {NODE, 32});
    const auto epoch = io_->dataMutationEpoch(); Stats stats;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&stats, Stats::observe);
        ASSERT_NO_THROW(run(admission(memory)));
    }
    EXPECT_EQ(stats.prepared, 1); EXPECT_EQ(stats.insufficient, 1); EXPECT_GT(stats.candidate, stats.row * 9 / 10);
    EXPECT_GE(stats.peak, stats.candidate); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_GT(io_->dataMutationEpoch(), epoch); // private COW preparation, never a read-only prediction
    EXPECT_EQ(properties_->readEntity(old, {NODE, 32}), expected);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 32}), expected); EXPECT_EQ(memory->used(), 0U);
    ASSERT_NO_THROW(replaceRow(1, PropertyValue(int64_t(777)), *properties_));
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(777)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyAllocationAdmissionTest, UnknownSourceCostsKeepRowBeforeAnyPrivateWrite) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = admission(memory);
    conversion.conversion->allocation_admission->census.maximum_catalog_entries = 0;
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch(); Stats stats;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&stats, Stats::observe); ASSERT_NO_THROW(run(conversion));
    }
    EXPECT_EQ(stats.unknown, 1); EXPECT_EQ(stats.prepared, 0); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyAllocationAdmissionTest, PeakLimitIsCheckedAgainstPreparedAllocationsBeforePublication) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = admission(memory);
    // The baseline fits1MiB; complete private candidate allocations exceed it.
    conversion.conversion->allocation_admission->maximum_admitted_peak_bytes = 1024 * 1024;
    const auto before = metadata_->openSnapshot().superblock(); Stats stats;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&stats, Stats::observe); ASSERT_NO_THROW(run(conversion));
    }
    EXPECT_EQ(stats.prepared, 1); EXPECT_EQ(stats.resource, 1); EXPECT_EQ(stats.insufficient, 0);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyAllocationAdmissionTest, DeclinedContentIndexAppendIsRecoveredBeforeTheNextOrdinaryRowWrite) {
    seed(NODE, 2, 0); LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    ASSERT_NO_THROW(replaceRow(1, PropertyValue(std::string(8192, 'x')), roots));
    ASSERT_NO_THROW(replaceRow(2, PropertyValue(std::string(8192, 'y')), roots));
    auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto expected = roots.readEntity(old, {NODE, 2});
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); Stats stats;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&stats, Stats::observe); ASSERT_NO_THROW(run(admission(memory)));
    }
    EXPECT_EQ(stats.prepared, 1); EXPECT_EQ(stats.insufficient, 1); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    ASSERT_NO_THROW(replaceRow(1, PropertyValue(int64_t(999)), roots));
    EXPECT_EQ(roots.readEntity(old, {NODE, 2}), expected); EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 2}), expected);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyAllocationAdmissionTest, InvalidAdmissionLimitsAreRejectedBeforePrivatePreparation) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = admission(memory);
    conversion.conversion->allocation_admission->maximum_admitted_file_bytes = 0;
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW(run(conversion), FormatException); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyAllocationAdmissionTest, FileHighWaterLimitCannotBeReplacedByTheSmallerAllocatedByteCount) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = admission(memory);
    conversion.conversion->allocation_admission->maximum_admitted_file_bytes = io_->fileSize();
    const auto before = metadata_->openSnapshot().superblock(); Stats stats;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&stats, Stats::observe); ASSERT_NO_THROW(run(conversion));
    }
    EXPECT_EQ(stats.prepared, 1); EXPECT_EQ(stats.resource, 1); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyAllocationAdmissionTest, CancellationAfterCompleteCostPreparationPreservesRowAndPermitsAnOrdinaryRetry) {
    struct Cancel final : CancellationCheckpoint {
        bool prepared = false;
        void check() const override { if (prepared) throw std::runtime_error("private admission cancelled"); }
    } cancellation;
    seed(NODE, 64, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto conversion = admission(memory);
    conversion.conversion->cancellation = &cancellation; auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto expected = properties_->readEntity(old, {NODE, 32});
    {
        graph::debug::PerfTrace::ScopedObserver observe(&cancellation, [](void *owner, std::string_view key, int64_t, bool) noexcept {
            if (key == "native_graph.hybrid.allocation_costs_prepared") static_cast<Cancel *>(owner)->prepared = true;
        });
        EXPECT_THROW(run(conversion), std::runtime_error);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
    ASSERT_NO_THROW(replaceRow(1, PropertyValue(int64_t(888)), *properties_));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 32}), expected);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyAllocationAdmissionTest, PrivatePreparedAccountingMatchesActualPublicationFromTheSameRecoveredSourceBytes) {
    seed(NODE, 64, 16); reopenNativeStores();
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{std::filesystem::path(path_.string() + ".allocation-control")};
    file_->flush(); ASSERT_TRUE(std::filesystem::copy_file(path_, cleanup.path, std::filesystem::copy_options::overwrite_existing));
    auto otherFile = std::make_shared<std::fstream>(cleanup.path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(otherFile->is_open());
    auto otherIO = std::make_shared<StorageIO>(otherFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto other = RelocatableMetadataStore::open(otherIO); auto old = metadata_->openSnapshot(); auto source = other->openSnapshot();
    ASSERT_EQ(old.superblock(), source.superblock());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto gated = admission(memory); auto ungated = gated;
    ungated.conversion->allocation_admission.reset(); ASSERT_NO_THROW(run(ungated));
    const auto actual = NativeStorageAllocationCensus(io_, *metadata_, memory).inspect(metadata_->openSnapshot());
    ASSERT_TRUE(actual); ASSERT_TRUE(actual->fullyAttributed()); Stats stats;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&stats, Stats::observe);
        const auto &state = source.superblock();
        ASSERT_NO_THROW((void)other->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, {},
            LogSequenceNumber(state.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {}, [](auto &) {},
            {}, {}, {}, {}, {}, {}, {}, state.generation, {}, {}, {}, {}, {}, gated));
    }
    EXPECT_EQ(stats.prepared, 1); EXPECT_EQ(uint64_t(stats.candidate), actual->steadyAllocatedBytes());
    EXPECT_EQ(uint64_t(stats.peak), actual->totalAllocatedBytes()); EXPECT_EQ(other->openSnapshot().superblock(), source.superblock());
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(otherIO, *other, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyAllocationAdmissionTest, PrivatePreparationCannotBypassTheExistingStorageIOHardFileLimit) {
    seed(NODE, 64, 16); const auto hardLimit = io_->fileSize();
    vector_manifests_.reset(); vector_pages_.reset(); properties_.reset(); labels_.reset(); btree_pages_.reset(); slotted_pages_.reset(); entities_.reset(); metadata_.reset(); io_.reset();
    io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE, StorageIOAccessMode::SIOAM_READ_WRITE, hardLimit);
    reopenNativeStores(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto before = metadata_->openSnapshot().superblock();
    EXPECT_THROW(run(admission(memory)), StorageFileLimitExceeded);
    EXPECT_LE(io_->fileSize(), hardLimit); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
    ASSERT_NO_THROW(replaceRow(1, PropertyValue(int64_t(444)), *properties_));
    EXPECT_LE(io_->fileSize(), hardLimit); EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyAllocationAdmissionTest, DeclinedRowContentAppendIsRecoveredAfterMetadataOnlyWritableReopen) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
        ASSERT_NO_THROW(replaceRow(1, PropertyValue(std::string(8192, 'x')), roots));
        ASSERT_NO_THROW(replaceRow(2, PropertyValue(std::string(8192, 'y')), roots));
        const auto before = metadata_->openSnapshot().superblock(); ASSERT_NO_THROW(run(admission(memory)));
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    }
    vector_manifests_.reset(); vector_pages_.reset(); properties_.reset(); labels_.reset(); btree_pages_.reset(); slotted_pages_.reset(); entities_.reset(); metadata_.reset();
    auto reopened = RelocatableMetadataStore::open(io_); const auto state = reopened->openSnapshot().superblock();
    ASSERT_FALSE(state.hybrid_property_descriptor_root.isValid());
    // No writable page-store constructor runs before this unrelated publication.
    ASSERT_NO_THROW((void)reopened->setPhysicalSlotAllocated(PhysicalSlotId(200), true, LogSequenceNumber(state.checkpoint_lsn.value() + 1U)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyAllocationAdmissionTest, DirtyPrivateCachePagesAreFlushedBeforeTheStableEpochCostObservation) {
    seed(NODE, 64, 16); io_->enableBufferPool(32U * RSS_PAGE_SIZE);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); const auto before = metadata_->openSnapshot().superblock();
    struct Capture { Stats stats; StorageIO *io; size_t dirty = SIZE_MAX; } capture{{}, io_.get()};
    {
        graph::debug::PerfTrace::ScopedObserver observe(&capture, [](void *owner, std::string_view key, int64_t value, bool timer) noexcept {
            auto &capture = *static_cast<Capture *>(owner); Stats::observe(&capture.stats, key, value, timer);
            if (key == "native_graph.hybrid.allocation_costs_prepared") capture.dirty = capture.io->bufferPoolStats().dirty_bytes;
        });
        ASSERT_NO_THROW(run(admission(memory)));
    }
    EXPECT_EQ(capture.stats.prepared, 1); EXPECT_EQ(capture.dirty, 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    ASSERT_NO_THROW(replaceRow(1, PropertyValue(int64_t(333)), *properties_));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
