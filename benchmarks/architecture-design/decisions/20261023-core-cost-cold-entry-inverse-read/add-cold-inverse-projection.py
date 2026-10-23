from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp');s=p.read_text();s=s.replace('    friend class HybridPropertyDemotionPreparation;', '    friend class HybridPropertyDemotionPreparation;\n    friend class HybridPropertyRowPreparation;',1);p.write_text(s)
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyRowPreparation.hpp');s=p.read_text();old='const auto group = context.groupRecord({kind, first}, std::ref(boundary));';new='// Read-only inverse projection supports cold roots without granting\n        // the publication coordinator mutation authority.\n        const auto group = context.findGroup({kind, first}, std::ref(boundary), nullptr, true, true);';assert old in s;s=s.replace(old,new,1);p.write_text(s)
p=Path('src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp');s=p.read_text();needle='    HybridPropertyRowPreparation projection(context, source, *this, entities, conversion.owner_kind,';assert needle in s;s=s.replace(needle,'''    // Retain the original public mutation guard until cold inverse directory,
    // ownership and retirement installation are complete. Read-only projection
    // alone must not enable publication of this new mode.
    static_cast<void>(context.groupRecord({conversion.owner_kind,
        (conversion.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + 1U}));
'''+needle,1);p.write_text(s)
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();s=s.replace('#include "graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp"', '#include "graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp"\n#include "graph/storage/relocatable/metadata/HybridPropertyRowPreparation.hpp"',1);a=s.rindex('#endif');s=s[:a]+'''
TEST_F(HybridPropertyColdCompatibleHotTest, ColdInverseProjectionRetainsActualLargeNullAndPropertylessNodeRows) {
    seed(true); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const HybridPropertyRowPreparation projection(context, pin, *metadata_, *entities_, NODE, 1, memory, values_.get());
        ASSERT_EQ(projection.rows().size(), 64U); EXPECT_EQ(projection.sourceGroup().memberships, 51U);
        size_t fields = 0;
        for (size_t i = 0; i < projection.rows().size(); ++i) {
            const auto &row = projection.rows()[i]; EXPECT_EQ(row.owner_entity_id, i + 1U);
            EXPECT_EQ(row.owner_entity_type, graph::Node::typeId); fields += row.entries.size();
        }
        EXPECT_EQ(fields, 51U); ASSERT_EQ(projection.rows()[0].entries.size(), 3U);
        EXPECT_EQ(projection.rows()[0].entries[2].key_token, 4U);
        EXPECT_EQ(std::get<std::string>(projection.rows()[0].entries[2].value.getVariant()).size(), 8193U);
        ASSERT_EQ(projection.rows()[2].entries.size(), 1U);
        EXPECT_TRUE(std::holds_alternative<std::monostate>(projection.rows()[2].entries[0].value.getVariant()));
        EXPECT_TRUE(projection.rows()[48].entries.empty());
        EXPECT_THROW((void)context.groupRecord({NODE, 1}), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdInverseProjectionPreservesEdgeOwnerNamespaceAndExactCounts) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const HybridPropertyRowPreparation projection(context, pin, *metadata_, *entities_, owner_kind_, 1, memory);
        ASSERT_EQ(projection.rows().size(), 64U); size_t fields = 0;
        for (const auto &row : projection.rows()) { EXPECT_EQ(row.owner_entity_type, graph::Edge::typeId); fields += row.entries.size(); }
        EXPECT_EQ(fields, 51U); EXPECT_EQ(projection.rows().back().owner_entity_id, 64U);
        EXPECT_EQ(projection.sourceGroup().owner_kind, owner_kind_);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdInverseProjectionRetainsAllPropertylessLiveOwners) {
    cold_only_ = true; omit_cold_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const HybridPropertyRowPreparation projection(context, pin, *metadata_, *entities_, NODE, 1, memory);
        ASSERT_EQ(projection.rows().size(), 64U);
        for (const auto &row : projection.rows()) EXPECT_TRUE(row.entries.empty());
        EXPECT_EQ(projection.sourceGroup().memberships, 0U); EXPECT_EQ(projection.sourceGroup().live, UINT64_MAX);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdInverseProjectionRejectsAccessedCorruptionAndRefunds) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        EXPECT_THROW((void)HybridPropertyRowPreparation(context, pin, *metadata_, *entities_, NODE, 1, memory), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[a:];p.write_text(s)
