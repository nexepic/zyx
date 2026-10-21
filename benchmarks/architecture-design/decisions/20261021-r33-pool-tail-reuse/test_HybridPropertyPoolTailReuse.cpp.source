#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
namespace {
class HybridPropertyPoolTailReuseTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {};
}
TEST_F(HybridPropertyPoolTailReuseTest, ConversionFillsPartialNativePoolBeforeSpanningAnotherPool) {
    constexpr size_t groups = 3, fields = 64;
    seed(NODE, groups * 64, 0);
    NativeGraphWritePlannerOptions placement; placement.first_slot = PhysicalSlotId(40); placement.last_slot = PhysicalSlotId(80);
    placement.entity_pool_owner_id = 901; placement.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement);
    const auto expected = [](uint64_t id) {
        PropertyCollectionRoot root{id, graph::Node::typeId, {}};
        for (uint32_t token = 1; token <= fields; ++token) root.entries.push_back({token, PropertyValue(int64_t(id + token))});
        return root;
    };
    for (size_t group = 0; group < groups; ++group) {
        auto tx = writer.beginTransaction();
        for (uint64_t id = group * 64 + 1; id <= (group + 1) * 64; ++id) tx.replaceNodeProperty(expected(id));
        ASSERT_TRUE(tx.commit().committed());
    }
    auto row = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    for (uint64_t group = 1; group <= 2; ++group) {
        auto conversion = request(NODE, memory); conversion.conversion->group_id = group;
        conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    }
    auto before = metadata_->openSnapshot();
    HybridPropertyReadContext reader(io_, *slotted_pages_, *metadata_, before);
    SlottedSlotReference reference;
    ASSERT_TRUE(reader.groupRecord({NODE, 1}, {}, &reference));
    const auto location = metadata_->findPageLocation(before, reference.page_id);
    const auto pool = metadata_->findCatalog(before, location.segment_id);
    SharedPagePoolAllocator poolReader(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    const auto freeFrames = poolReader.availableFrameCount(pool);
    ASSERT_GT(freeFrames, 0); ASSERT_LT(freeFrames, RSS_PAGE_POOL_DATA_FRAME_COUNT);
    auto conversion = request(NODE, memory); conversion.conversion->group_id = 3;
    conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    auto after = metadata_->openSnapshot();
    EXPECT_EQ(poolReader.availableFrameCount(pool), 0U);
    EXPECT_EQ(metadata_->findPageLocation(after, reference.page_id), location);
    // The last group's fields span the partial old pool and a new pool. Both
    // old native and ROW pins retain their exact values and physical mappings.
    for (const uint64_t id : {1U, 65U, 129U, 192U}) {
        EXPECT_EQ(properties_->readEntity(row, {NODE, id}), expected(id));
        EXPECT_EQ(properties_->readEntity(before, {NODE, id}), expected(id));
        EXPECT_EQ(properties_->readEntity(after, {NODE, id}), expected(id));
    }
    EXPECT_NO_THROW((void)HybridPropertyDirectoryIntegrity::verify(
        HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, after), after, memory));
    EXPECT_EQ(memory->used(), 0U);
}
#endif
