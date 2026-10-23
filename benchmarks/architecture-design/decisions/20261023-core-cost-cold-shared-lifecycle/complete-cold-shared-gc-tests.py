from pathlib import Path
r=Path('/workspace/zyx');p=r/'tests/include/storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp';s=p.read_text();s=s.replace('''            auto otherGroup = group; otherGroup.owner_kind = otherKind; otherGroup.memberships = hot_owner_count_ + 3U;''','''            auto otherGroup = group; otherGroup.mode = HybridPropertyGroupMode::HPGM_HYBRID;
            otherGroup.owner_kind = otherKind; otherGroup.memberships = hot_owner_count_ + 3U;''',1)
s=s.replace('''            otherGroup.column_membership = image.appendColumnMembership(otherMatrix, otherGroup, current.generation);
            otherGroupSlot''','''            otherGroup.column_membership = image.appendColumnMembership(otherMatrix, otherGroup, current.generation);
            otherGroup.mode = HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;
            otherGroupSlot''',1);p.write_text(s)
p=r/'tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp';s=p.read_text();s=s.replace('#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"','#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"\n#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"',1)
at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdNativeLifecycleTest, ActualColdValueReclamationPreservesLiveValueAndPinnedOldCarrierThenReusesFrames) {
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    const auto originalManifest = values_->manifest(old, value_reference_); ASSERT_FALSE(originalManifest.chunks.empty());
    const auto oldData = metadata_->findPageLocation(old, originalManifest.chunks.front().first_data_page_id);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const graph::OwnedPropertyValue changed(graph::PropertyValue("gc-id"));
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 2, changed}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    LargeValueReachabilityReclaimOptions options; options.maximum_manifest_locations = 32;
    options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 32;
    options.maximum_owner_locations = 128; options.maximum_owner_metadata_pages = 128;
    options.maximum_data_pages = 64; options.maximum_retirements = 64; options.maximum_page_locations = 128;
    options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        LargeValueReachabilityReclaimer reclaimer(*values_, *slotted_pages_, *metadata_);
        const auto nextLsn = [&] { return LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1); };
        const auto retired = reclaimer.reclaim(options, nextLsn());
        EXPECT_EQ(retired.reachable_values, 1U); EXPECT_EQ(retired.retired_values, 1U);
        {
            auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findLargeValueManifestOptional(current, value_reference_.value_id));
            EXPECT_TRUE(metadata_->isPhysicalSlotAllocated(current, oldData.segment_id.isValid() ?
                metadata_->findCatalog(old, oldData.segment_id).physical_slot : PhysicalSlotId{}));
            HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old), context(io_, *slotted_pages_, *metadata_, current);
            const auto original = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()); ASSERT_TRUE(original.value);
            EXPECT_EQ(original.value->value, graph::PropertyValue(std::string(8193, 'x')));
            const auto live = HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()); ASSERT_TRUE(live.value);
            EXPECT_EQ(live.value->value, graph::PropertyValue("gc-id"));
        }
        old.release(); const auto released = reclaimer.reclaim(options, nextLsn()); EXPECT_EQ(released.retired_values, 0U);
        auto current = metadata_->openSnapshot(); const auto pool = metadata_->findCatalog(current, oldData.segment_id);
        if (pool.state == SegmentState::SGS_SEALED) {
            const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.superblock().data_region_offset, pool);
            EXPECT_FALSE(SharedPagePoolFrameMap::isAllocated(header, uint8_t(oldData.page_index)));
        } else EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(current, pool.physical_slot));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[at:];p.write_text(s)
