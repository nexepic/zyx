#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
using namespace graph::storage;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyLogicalIdsTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    void native() {
        seed(NODE); auto conversion = request(NODE, std::make_shared<MaterializedByteBudget>(1024 * 1024));
        conversion.conversion->publish_candidate = true; run(conversion);
    }
    void reopenForLeases() {
        vector_manifests_.reset(); vector_pages_.reset(); properties_.reset(); labels_.reset();
        btree_pages_.reset(); slotted_pages_.reset(); entities_.reset(); metadata_.reset();
        metadata_ = RelocatableMetadataStore::open(io_);
        entities_ = std::make_unique<EntityPageStore>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        btree_pages_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
        labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
    }
    void sameGraph(const Superblock &before, const Superblock &after) {
        EXPECT_EQ(after.node_entity_directory_root, before.node_entity_directory_root);
        EXPECT_EQ(after.edge_entity_directory_root, before.edge_entity_directory_root);
        EXPECT_EQ(after.entity_property_directory_root, before.entity_property_directory_root);
        EXPECT_EQ(after.hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
        EXPECT_EQ(after.index_root_directory_root, before.index_root_directory_root);
        EXPECT_EQ(after.large_value_manifest_directory_root, before.large_value_manifest_directory_root);
        EXPECT_EQ(after.vector_location_directory_root, before.vector_location_directory_root);
        EXPECT_EQ(after.vector_manifest_directory_root, before.vector_manifest_directory_root);
        EXPECT_EQ(after.state_directory_root, before.state_directory_root);
    }
};
}
TEST_F(HybridPropertyLogicalIdsTest, DefaultFormatRetainsOrdinaryLeaseContract) {
#if !ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    seed(NODE, 1, 1); const auto before = metadata_->openSnapshot().superblock();
    const auto lease = metadata_->reserveNativeGraphLogicalIds(2, 3);
    EXPECT_EQ(lease.nodes.first_id, before.next_logical_node_id); EXPECT_EQ(lease.edges.first_id, before.next_logical_edge_id);
    sameGraph(before, lease.superblock);
#endif
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyLogicalIdsTest, DurableLeasePreservesNativeFieldsGraphRootsAndOldPins) {
    native(); auto old = metadata_->openSnapshot(); const auto expected = properties_->readEntity(old, {NODE, 1});
    const auto before = old.superblock(); const auto lease = metadata_->reserveNativeGraphLogicalIds(2, 3);
    EXPECT_EQ(lease.nodes.first_id, before.next_logical_node_id); EXPECT_EQ(lease.nodes.count, 2U);
    EXPECT_EQ(lease.edges.first_id, before.next_logical_edge_id); EXPECT_EQ(lease.edges.count, 3U);
    sameGraph(before, lease.superblock);
    EXPECT_EQ(lease.superblock.next_logical_node_id, before.next_logical_node_id + 2);
    EXPECT_EQ(lease.superblock.next_logical_edge_id, before.next_logical_edge_id + 3);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), expected);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), expected);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, std::make_shared<MaterializedByteBudget>(1024 * 1024)).scan());
}
TEST_F(HybridPropertyLogicalIdsTest, CachedRangesAreSingleUseAndReopenSkipsUnissuedTails) {
    native(); const auto before = metadata_->openSnapshot().superblock();
    const auto first = metadata_->acquireNativeGraphLogicalIds(1, 1, 8);
    ASSERT_EQ(first.node_ranges.size(), 1U); ASSERT_EQ(first.edge_ranges.size(), 1U);
    EXPECT_EQ(first.node_ranges[0].first_id, before.next_logical_node_id);
    EXPECT_TRUE(metadata_->claimNativeGraphLogicalIdReservation(first));
    EXPECT_FALSE(metadata_->claimNativeGraphLogicalIdReservation(first));
    const auto leased = metadata_->openSnapshot().superblock(); sameGraph(before, leased);
    const auto second = metadata_->acquireNativeGraphLogicalIds(2, 3, 8);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), leased);
    ASSERT_EQ(second.node_ranges.size(), 1U); EXPECT_EQ(second.node_ranges[0].first_id, before.next_logical_node_id + 1);
    EXPECT_TRUE(metadata_->claimNativeGraphLogicalIdReservation(second));
    reopenForLeases(); const auto fresh = metadata_->acquireNativeGraphLogicalIds(1, 1, 8);
    ASSERT_EQ(fresh.node_ranges.size(), 1U); ASSERT_EQ(fresh.edge_ranges.size(), 1U);
    EXPECT_EQ(fresh.node_ranges[0].first_id, leased.next_logical_node_id);
    EXPECT_EQ(fresh.edge_ranges[0].first_id, leased.next_logical_edge_id);
    sameGraph(leased, metadata_->openSnapshot().superblock());
}
TEST_F(HybridPropertyLogicalIdsTest, BootstrapInterruptionCannotExposeDuplicateLeasesOrChangeNativeRoots) {
    native();
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        const auto before = metadata_->openSnapshot().superblock(); MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)metadata_->reserveNativeGraphLogicalIds(2, 3, options), BootstrapPublishInterrupted);
        reopenForLeases(); const auto recovered = metadata_->openSnapshot().superblock(); sameGraph(before, recovered);
        const bool published = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(recovered.next_logical_node_id, before.next_logical_node_id + (published ? 2U : 0U));
        EXPECT_EQ(recovered.next_logical_edge_id, before.next_logical_edge_id + (published ? 3U : 0U));
        const auto lease = metadata_->reserveNativeGraphLogicalIds(1, 1);
        EXPECT_EQ(lease.nodes.first_id, recovered.next_logical_node_id); EXPECT_EQ(lease.edges.first_id, recovered.next_logical_edge_id);
        EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries.size(), 2U);
    }
}
TEST_F(HybridPropertyLogicalIdsTest, ReadOnlyLeaseRefusalAndGeneralWriterGuardRemain) {
    native(); auto readonly = RelocatableMetadataStore::openReadOnly(io_); const auto before = metadata_->openSnapshot().superblock();
    const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW((void)readonly->reserveNativeGraphLogicalIds(1, 1), FormatException);
    EXPECT_THROW((void)readonly->acquireNativeGraphLogicalIds(1, 1, 8), FormatException);
    EXPECT_THROW((void)SharedPagePoolAllocator(io_, *metadata_), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyLogicalIdsTest, HighWaterLeasesDoNotGrantGeneralGraphMutationAuthority) {
    native(); NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto tx = planner.beginTransaction(); const auto created = tx.createNode({});
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    try { (void)tx.commit(); FAIL() << "ordinary graph publication bypassed the active native guard"; }
    catch (const FormatException &error) { EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION); }
    const auto current = metadata_->openSnapshot(); sameGraph(before, current.superblock());
    EXPECT_EQ(current.superblock().generation, before.generation); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_THROW((void)entities_->readNode(current, created), FormatException);
    EXPECT_EQ(properties_->readEntity(current, {NODE, 1}).entries.size(), 2U);
}
#endif
