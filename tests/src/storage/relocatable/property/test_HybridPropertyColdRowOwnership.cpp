#include "storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
using namespace graph::storage;
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
using graph::storage::test::HybridPropertyColdRowRoutingTest;
class HybridPropertyColdRowOwnershipTest : public HybridPropertyColdRowRoutingTest {};
namespace graph::storage {
class EntityPropertyLargeValueOwnershipScannerTestAccess {
public:
    static LargeValueOwnershipScanner::Progress scan(const EntityPropertyLargeValueOwnershipScanner &scanner,
        const HybridPropertyReadContext &context, const MetadataSnapshotPin &pin, uint64_t after,
        size_t owners, size_t pages, LargeValueOwnershipScanSink &sink) {
        return scanner.scanReachableAfterImpl(pin, after, owners, pages, sink, &context);
    }
};
}
namespace {
struct ColdMarkSink final : LargeValueOwnershipScanSink {
    const LargeValueStore &values; const MetadataSnapshotPin &pin; size_t calls = 0;
    explicit ColdMarkSink(const LargeValueStore &v, const MetadataSnapshotPin &p) : values(v), pin(p) {}
    void markReachable(const LargeValueRef &reference) override {
        static_cast<void>(values.manifest(pin, reference)); ++calls;
    }
};
struct StopColdAudit final : CancellationCheckpoint {
    void check() const override { throw std::runtime_error("cold audit cancelled"); }
};
}
TEST_F(HybridPropertyColdRowOwnershipTest, ForwardAndReverseAuditDistinctRawBlocksWithoutMaterializingValues) {
    raw_cold_ = split_cold_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) { throw std::logic_error("audit cannot decode PropertyValue"); });
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 51U); EXPECT_EQ(report.columns, 1U);
    EXPECT_EQ(report.cold_blocks, 2U); EXPECT_EQ(report.cold_rows, 2U); EXPECT_EQ(report.large_values, 0U);
    EXPECT_EQ(report.extensions, 0U); EXPECT_EQ(report.directory_selectors, 0U);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory), 5U);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, ColdOnlyGroupsKeepExactOwnerAndPhysicalRecordOwnership) {
    cold_only_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 3U); EXPECT_EQ(report.columns, 0U);
    EXPECT_EQ(report.cold_blocks, 1U); EXPECT_EQ(report.cold_rows, 2U);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory), 3U);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, EdgeNamespaceRawColdRecordsAndRealLargeRootsEnumerateTheirExactReferenceOnce) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; raw_cold_ = true; seed(true); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) { throw std::logic_error("audit cannot materialize values"); });
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory, values_.get());
    EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.large_values, 1U); EXPECT_EQ(report.memberships, 51U);
    size_t references = 0;
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory, values_.get(),
        [&](const LargeValueRef &reference) { EXPECT_EQ(reference, value_reference_); ++references; }), 4U);
    EXPECT_EQ(references, 1U); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, IncorrectOwnerToBlockBindingCannotPassForwardOrReverseChecks) {
    wrong_cold_binding_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, GlobalColdCountIsSummedAcrossBlocksRatherThanTrustedFromLocalBounds) {
    split_cold_ = wrong_cold_count_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    // Every local block/ref is valid. Only a full group count establishes the
    // declared global total; a reverse-page check cannot replace that phase.
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory), 5U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, UnreferencedDuplicateColdRecordRequiresTheReversePhysicalPhase) {
    orphan_cold_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).cold_blocks, 1U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, UnreferencedDuplicateMembershipDoesNotAcquireItsGroupsAuthority) {
    orphan_matrix_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).groups, 1U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, CompleteAuditInspectsActualValuesBeyondCanonicalDirectoriesAndValidOuterChecksums) {
    malformed_cold_value_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    { graph::utils::OwnedAllocationScope scope(memory);
      EXPECT_EQ(context.count({owner_kind_, 1}), 3U); } // directory/count alone is valid
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), FormatException);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, LargeReferenceMustNotBeNewerThanItsColdBlockEvenWhenTheGroupIsNewer) {
    future_cold_reference_ = true; seed(true); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    // The stored value's logical generation is independent of metadata epochs.
    // The forged reference is <=group generation but >block generation. It
    // must fail locally as INVALID_HEADER, before the actual manifest's
    // different immutable identity could fail later as INVALID_STATE.
    const auto localRejection = [&](const auto &audit) {
        try { audit(); ADD_FAILURE() << "future cold reference was accepted"; }
        catch (const FormatException &error) { EXPECT_EQ(error.error(), FormatError::FERR_INVALID_HEADER); }
    };
    localRejection([&] { static_cast<void>(HybridPropertyDirectoryIntegrity::verify(context, pin, memory, values_.get())); });
    localRejection([&] { static_cast<void>(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory, values_.get())); });
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, ActualLargeStoreIsRequiredAndReferenceConsumerExceptionsDoNotReplayOrWrite) {
    seed(true); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto bytes = io_->fileSize(), epoch = io_->dataMutationEpoch();
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U); size_t calls = 0;
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory, values_.get(),
        [&](const auto &) { ++calls; throw std::runtime_error("reference consumer"); }), std::runtime_error);
    EXPECT_EQ(calls, 1U); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
}
TEST_F(HybridPropertyColdRowOwnershipTest, QuotaAndCancellationRefuseWithoutLosingTemporaryOwnershipAndCanRetry) {
    seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, tiny), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, tiny), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    StopColdAudit stop;
    auto cancelled = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_, &stop);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(cancelled, pin, memory), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).memberships, 51U);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, field_page_id_, memory), 4U);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, ReferenceMarkingBorrowsOneWholeRootWithoutFieldMaterialization) {
    seed(true); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    size_t references = 0;
    const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) { throw std::logic_error("mark cannot decode values"); });
        EXPECT_TRUE(context.visitColdReference({owner_kind_, 1}, [&](const LargeValueRef &reference) {
            EXPECT_EQ(reference, value_reference_); ++references;
            // The real maintenance sink retains only an authenticated manifest.
            static_cast<void>(values_->manifest(pin, reference));
        }));
        EXPECT_FALSE(context.visitColdReference({owner_kind_, 64}, [&](const auto &) { ++references; }));
        EXPECT_FALSE(context.visitColdReference({owner_kind_, 2}, [&](const auto &) { ++references; }));
    }
    EXPECT_EQ(references, 1U); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdRowOwnershipTest, ReferenceMarkingRejectsFutureBlockReferenceBeforeCallingSink) {
    future_cold_reference_ = true; seed(true); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    size_t references = 0;
    { graph::utils::OwnedAllocationScope scope(memory);
      EXPECT_THROW(context.visitColdReference({owner_kind_, 1}, [&](const auto &) { ++references; }), FormatException); }
    EXPECT_EQ(references, 0U); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, ReferenceSinkFailureDoesNotReplayAndReleasesItsWindow) {
    seed(true); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    size_t references = 0;
    { graph::utils::OwnedAllocationScope scope(memory);
      EXPECT_THROW(context.visitColdReference({owner_kind_, 1}, [&](const auto &) {
          ++references; throw std::runtime_error("mark sink rejected manifest");
      }), std::runtime_error); }
    EXPECT_EQ(references, 1U); EXPECT_EQ(memory->used(), 0U);
    { graph::utils::OwnedAllocationScope scope(memory);
      EXPECT_TRUE(context.visitColdReference({owner_kind_, 1}, [&](const auto &) { ++references; })); }
    EXPECT_EQ(references, 2U); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, ScannerResumesAfterOneWholeRootAndOneReferenceFreeBoundary) {
    seed(true); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EntityPropertyLargeValueOwnershipScanner scanner(*slotted_pages_, *metadata_);
    ColdMarkSink sink(*values_, pin);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        const auto first = EntityPropertyLargeValueOwnershipScannerTestAccess::scan(scanner, context, pin, 0, 1, 4096, sink);
        EXPECT_FALSE(first.exhausted); EXPECT_EQ(first.locations_scanned, 1U);
        EXPECT_EQ(first.last_scanned_location_id, (EntityPropertyKey{owner_kind_, 1}.directoryId())); EXPECT_EQ(sink.calls, 1U);
        const auto rest = EntityPropertyLargeValueOwnershipScannerTestAccess::scan(scanner, context, pin,
            first.last_scanned_location_id, 1, 4096, sink);
        EXPECT_TRUE(rest.exhausted); EXPECT_EQ(rest.locations_scanned, 1U);
        EXPECT_EQ(rest.last_scanned_location_id, (EntityPropertyKey{owner_kind_, 64}.directoryId())); EXPECT_EQ(sink.calls, 1U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdRowOwnershipTest, ScannerAdvancesInlineColdOnlyEdgeGroupWithoutFieldEnumeration) {
    cold_only_ = true; owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    EntityPropertyLargeValueOwnershipScanner scanner(*slotted_pages_, *metadata_); ColdMarkSink sink(values, pin);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    { graph::utils::OwnedAllocationScope scope(memory);
      const auto result = EntityPropertyLargeValueOwnershipScannerTestAccess::scan(scanner, context, pin, 0, 1, 4096, sink);
      EXPECT_TRUE(result.exhausted); EXPECT_EQ(result.locations_scanned, 1U);
      EXPECT_EQ(result.last_scanned_location_id, (EntityPropertyKey{owner_kind_, 64}.directoryId())); }
    EXPECT_EQ(sink.calls, 0U); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowOwnershipTest, ScannerCannotGrantExhaustionWhenMetadataOrOwnerBudgetIsAbsent) {
    seed(true); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EntityPropertyLargeValueOwnershipScanner scanner(*slotted_pages_, *metadata_); ColdMarkSink sink(*values_, pin);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    { graph::utils::OwnedAllocationScope scope(memory);
      for (size_t pages : {size_t{0}, size_t{1}}) {
          const auto result = EntityPropertyLargeValueOwnershipScannerTestAccess::scan(scanner, context, pin, 0, 1, pages, sink);
          EXPECT_FALSE(result.exhausted); EXPECT_EQ(result.locations_scanned, 0U); EXPECT_EQ(result.last_scanned_location_id, 0U);
          EXPECT_LE(result.metadata_pages_scanned, pages);
      }
      const auto result = EntityPropertyLargeValueOwnershipScannerTestAccess::scan(scanner, context, pin, 0, 0, 4096, sink);
      EXPECT_FALSE(result.exhausted); EXPECT_EQ(result.locations_scanned, 0U); }
    EXPECT_EQ(sink.calls, 0U); EXPECT_EQ(memory->used(), 0U);
}
#endif
