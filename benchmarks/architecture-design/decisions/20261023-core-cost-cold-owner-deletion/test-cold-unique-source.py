from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();s=s.replace('#include "graph/storage/relocatable/schema/SystemCatalog.hpp"','#include "graph/storage/relocatable/schema/SystemCatalog.hpp"\n#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"',1);at=s.rfind('#endif')
s=s[:at]+'''TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdUniqueChecksAndOwnerDeletionUseTheSameAuthenticatedSource) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    SystemCatalog catalog(*btree_pages_, *metadata_); uint64_t person = 0, support = 0;
    {
        auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
        EXPECT_EQ(schema.ensurePropertyToken("hot"), 1U); EXPECT_EQ(schema.ensurePropertyToken("cold"), 2U);
        EXPECT_EQ(schema.ensurePropertyToken("unused"), 3U); EXPECT_EQ(schema.ensurePropertyToken("nullable"), 4U);
        person = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
        schema.setGraphCardinalityStatistics({64, 0}); schema.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    {
        auto tx = planner.beginTransaction(); const std::array names{person};
        tx.replaceNodeLabels(1, names); tx.replaceNodeLabels(64, names); ASSERT_TRUE(tx.commit().committed());
    }
    std::vector<BTreePageEntry> keys;
    {
        auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
        ConstraintDefinition unique; unique.name = "unique_cold"; unique.kind = ConstraintDefinitionKind::CDK_UNIQUE;
        unique.schema_token_id = person; unique.property_names = {"cold"};
        unique = schema.createConstraintDefinition(unique); support = unique.constraint_id; schema.finish();
        for (const uint64_t id : {1U, 64U}) {
            const std::array<PropertyValue, 1> values{PropertyValue(id == 1 ? "user-000000" : "user-000063")};
            const auto key = ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), id);
            keys.push_back({graph::utils::OwnedStdVector<std::byte>(key.begin(), key.end()), id});
        }
        std::ranges::sort(keys, {}, &BTreePageEntry::key);
        tx.stageScalarIndexCreate(*btree_pages_, support, keys); ASSERT_TRUE(tx.commit().committed());
    }
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    descriptor_.semantic_generation = before.generation; auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue duplicate(PropertyValue("user-000000"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 64}, 2, duplicate}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const std::runtime_error &error) {
            EXPECT_NE(std::string(error.what()).find("Constraint violation: 'unique_cold'"), std::string::npos) << error.what();
        }
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}}; request.replacements = {}; request.owner_deletions = deleted;
        int64_t constraints = 0;
        graph::debug::PerfTrace::ScopedObserver observer(&constraints, [](void *p, std::string_view key, int64_t n, bool) noexcept {
            if (key == "native_graph.hybrid.private_constraint_index_images") *static_cast<int64_t *>(p) = n;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_GT(constraints, 0);
        for (const auto &entry : keys) EXPECT_EQ(BTreeIndex(support, *btree_pages_, *metadata_).find(pin, entry.key),
            std::optional(entry.value_or_child));
        EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[at:];p.write_text(s)
