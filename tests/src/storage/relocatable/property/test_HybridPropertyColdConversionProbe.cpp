#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyColdConversionProbeTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    struct Work { int64_t fields = -1, indexes = -1, roots = 0, values = -1, new_values = -1, data = -1, manifests = -1; };
    Work probe(const Request &request) {
        Work work;
        graph::debug::PerfTrace::ScopedObserver observer(&work, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            auto &w = *static_cast<Work *>(p);
            if (key == "native_graph.hybrid.private_field_frames") w.fields = value;
            if (key == "native_graph.hybrid.private_index_frames") w.indexes = value;
            if (key == "native_graph.hybrid.large_root_proposals_prepared") w.roots += value;
            if (key == "native_graph.hybrid.private_large_values") w.values = value;
            if (key == "native_graph.hybrid.private_new_large_values") w.new_values = value;
            if (key == "native_graph.hybrid.private_large_data_frames") w.data = value;
            if (key == "native_graph.hybrid.private_large_manifest_frames") w.manifests = value;
        });
        expectPreparedRefusal(request); return work;
    }
    void verifyMixed(EntityPropertyOwnerKind kind, bool large) {
        seed(kind, 64, 0);
        LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, options);
        auto tx = planner.beginTransaction();
        for (uint64_t id = 1; id <= 64; ++id) {
            PropertyCollectionRoot row{id, kind == NODE ? graph::Node::typeId : graph::Edge::typeId,
                {{1, PropertyValue(int64_t(id))}, {2, large && id == 1 ? PropertyValue(std::string(8193, 'x')) :
                    PropertyValue("user-" + std::to_string(100000U + id))}}};
            if (kind == NODE) tx.replaceNodeProperty(std::move(row)); else tx.replaceEdgeProperty(std::move(row));
        }
        ASSERT_TRUE(tx.commit().committed());
        const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
        const auto epoch = io_->dataMutationEpoch(); const auto size = std::filesystem::file_size(path_);
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        auto conversion = request(kind, memory); conversion.conversion->compact_column_memberships = true;
        conversion.conversion->cold_row_remainder = true;
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
            EXPECT_THROW(run(conversion), FormatException);
        } else {
            const auto work = probe(conversion);
            EXPECT_EQ(work.fields, 1); EXPECT_EQ(work.indexes, 1); // Group directory only; no per-field cold index.
            EXPECT_EQ(work.values, large ? 1 : 0); EXPECT_EQ(work.new_values, large ? 1 : 0);
            EXPECT_EQ(work.roots, large ? 1 : 0);
            if (large) { EXPECT_GT(work.data, 0); EXPECT_GT(work.manifests, 0); }
            else { EXPECT_EQ(work.data, 0); EXPECT_EQ(work.manifests, 0); }
        }
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(std::filesystem::file_size(path_), size);
        const auto old = roots.readEntity(pin, {kind, 1}); ASSERT_EQ(old.entries.size(), 2U);
        EXPECT_EQ(old.entries[0].value, PropertyValue(int64_t(1)));
        EXPECT_EQ(old.entries[1].value, large ? PropertyValue(std::string(8193, 'x')) : PropertyValue("user-100001"));
    }
};
}
TEST_F(HybridPropertyColdConversionProbeTest, ActualNodeSourceReachesColdPhysicalAndDirectoryPreparationWithoutWriting) { verifyMixed(NODE, false); }
TEST_F(HybridPropertyColdConversionProbeTest, ActualEdgeSourceReachesColdPreparationWithCorrectOwnerNamespace) { verifyMixed(EDGE, false); }
TEST_F(HybridPropertyColdConversionProbeTest, ChangedWholeRootPayloadPlansNewDataAndManifestInTheSameNativeBatchOnly) { verifyMixed(NODE, true); }
TEST_F(HybridPropertyColdConversionProbeTest, ColdPublicationAndUnsupportedProbeShapesRefuseBeforePreparingAnySource) {
    seed(NODE, 2, 0); const auto before = metadata_->openSnapshot().superblock();
    const auto epoch = io_->dataMutationEpoch(); const auto size = std::filesystem::file_size(path_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); size_t classifications = 0;
    graph::debug::PerfTrace::ScopedObserver observer(&classifications, [](void *p, std::string_view key, int64_t value, bool) noexcept {
        if (key == "native_graph.hybrid.source_classification_completed") *static_cast<size_t *>(p) += size_t(value);
    });
    auto conversion = request(NODE, memory); conversion.conversion->cold_row_remainder = true;
    conversion.conversion->compact_column_memberships = true; conversion.conversion->publish_candidate = true;
    EXPECT_THROW(run(conversion), FormatException);
    conversion.conversion->publish_candidate = false; conversion.conversion->compact_column_memberships = false;
    EXPECT_THROW(run(conversion), FormatException);
    conversion.conversion->compact_column_memberships = true; conversion.conversion->group_count = 2;
    EXPECT_THROW(run(conversion), FormatException);
    conversion.conversion->group_count = 1;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    EXPECT_THROW(run(conversion), FormatException);
    EXPECT_EQ(classifications, 0U); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(std::filesystem::file_size(path_), size);
}
