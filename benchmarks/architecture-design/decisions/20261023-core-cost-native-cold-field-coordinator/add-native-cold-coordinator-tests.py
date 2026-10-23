from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text().replace('#include <algorithm>','#include <algorithm>\n#include "storage/relocatable/property/HybridPropertyNativeWriteTestAccess.hpp"',1);a=s.rindex('#endif')
s=s[:a]+'''TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorPreparesCompatibleHotFieldsAndRefusesBeforePhysicalWrites) {
    seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto epoch = io_->dataMutationEpoch(); const auto originalBytes = io_->fileSize();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        for (const uint64_t owner : {1U, 49U}) {
            const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, owner}, 1, int64_t(444)}}};
            NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
                before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
            request.field_lifecycle = true;
            int64_t prepared = 0;
            graph::debug::PerfTrace::ScopedObserver observe(&prepared, [](void *p, std::string_view key, int64_t count, bool) noexcept {
                if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) += count;
            });
            try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
            catch (const FormatException &error) {
                EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
                EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
            }
            EXPECT_GT(prepared, 0);
            EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
            EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), originalBytes);
            EXPECT_THROW(context.groupRecord({NODE, owner}), FormatException);
        }
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorPrivateSourceRejectsAnotherPinAndAccessedEntityConflict) {
    competing_row_ = true; seed(); auto pin = metadata_->openSnapshot(); auto otherPin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(444)}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; int64_t prepared = 0;
        graph::debug::PerfTrace::ScopedObserver observe(&prepared, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) += count;
        });
        EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepare(*metadata_, otherPin, context, request), FormatException);
        EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request), FormatException);
        EXPECT_EQ(prepared, 0); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    }
    EXPECT_EQ(memory->used(), 0U);
}
''' +s[a:];p.write_text(s)
