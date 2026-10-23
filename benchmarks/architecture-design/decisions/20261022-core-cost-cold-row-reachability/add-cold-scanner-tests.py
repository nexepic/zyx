from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdRowOwnership.cpp');s=p.read_text().replace('#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT','#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"\n#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT',1);idx=s.index('namespace {');s=s[:idx]+'''namespace graph::storage {
class EntityPropertyLargeValueOwnershipScannerTestAccess {
public:
    static LargeValueOwnershipScanner::Progress scan(const EntityPropertyLargeValueOwnershipScanner &scanner,
        const HybridPropertyReadContext &context, const MetadataSnapshotPin &pin, uint64_t after,
        size_t owners, size_t pages, LargeValueOwnershipScanSink &sink) {
        return scanner.scanReachableAfterImpl(pin, after, owners, pages, sink, &context);
    }
};
}
''' +s[idx:];idx=s.index('struct StopColdAudit');s=s[:idx]+'''struct ColdMarkSink final : LargeValueOwnershipScanSink {
    const LargeValueStore &values; const MetadataSnapshotPin &pin; size_t calls = 0;
    explicit ColdMarkSink(const LargeValueStore &v, const MetadataSnapshotPin &p) : values(v), pin(p) {}
    void markReachable(const LargeValueRef &reference) override {
        static_cast<void>(values.manifest(pin, reference)); ++calls;
    }
};
''' +s[idx:];idx=s.rindex('#endif');s=s[:idx]+'''TEST_F(HybridPropertyColdRowOwnershipTest, ScannerResumesAfterOneWholeRootAndOneReferenceFreeBoundary) {
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
    EntityPropertyLargeValueOwnershipScanner scanner(*slotted_pages_, *metadata_); ColdMarkSink sink(*values_, pin);
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
''' +s[idx:];p.write_text(s)
# The new work contract explicitly counts the reference-free tail boundary as
# one unit, independently of the five references in the original test fixture.
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyLargeOwnership.cpp');s=p.read_text();s=s.replace('EXPECT_EQ(complete.locations_scanned, 5U)', 'EXPECT_EQ(complete.locations_scanned, 6U)');s=s.replace('EXPECT_EQ(afterFirst.locations_scanned, 3U)', 'EXPECT_EQ(afterFirst.locations_scanned, 4U)');s=s.replace('EXPECT_EQ(complete.locations_scanned, 3U)', 'EXPECT_EQ(complete.locations_scanned, 4U)');p.write_text(s)
for profile in ['hybrid','default']:
 src=Path('/tmp/build-cold-row-ownership-'+profile+'-modules.py').read_text()
 src=src.replace("units=[", "units=['property/test_HybridPropertyLargeOwnership', 'maintenance/test_LargeValueReachabilityReclaimer', 'maintenance/test_LargeValueMaintenanceJournal', ")
 Path('/tmp/build-cold-reachability-'+profile+'-modules.py').write_text(src)
