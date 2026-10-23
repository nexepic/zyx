from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text().replace('#include <algorithm>','#include <algorithm>\n#include "graph/storage/relocatable/schema/SystemCatalog.hpp"\n#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"\n#include "graph/storage/relocatable/index/BTreeIndex.hpp"',1);a=s.rindex('#endif')
s=s[:a]+'''TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorUsesColdSourceForRealCatalogIndexesAndRequiredFields) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    SystemCatalog catalog(*btree_pages_, *metadata_); uint64_t person = 0;
    {
        auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
        EXPECT_EQ(schema.ensurePropertyToken("hot"), 1U); EXPECT_EQ(schema.ensurePropertyToken("cold"), 2U);
        EXPECT_EQ(schema.ensurePropertyToken("unused"), 3U); EXPECT_EQ(schema.ensurePropertyToken("nullable"), 4U);
        person = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
        schema.setGraphCardinalityStatistics({64, 0}); schema.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    {
        auto tx = planner.beginTransaction(); const std::array labels{person}; tx.replaceNodeLabels(1, labels);
        ASSERT_TRUE(tx.commit().committed());
    }
    IndexDefinition hot, cold;
    {
        auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
        hot.name = "hot_values"; hot.kind = IndexDefinitionKind::IDK_PROPERTY; hot.property_names = {"hot"};
        cold.name = "cold_values"; cold.kind = IndexDefinitionKind::IDK_PROPERTY; cold.property_names = {"cold"};
        hot = schema.createIndexDefinition(hot); cold = schema.createIndexDefinition(cold);
        ConstraintDefinition required; required.name = "required_cold"; required.kind = ConstraintDefinitionKind::CDK_NOT_NULL;
        required.schema_token_id = person; required.property_names = {"cold"};
        static_cast<void>(schema.createConstraintDefinition(required)); schema.finish();
        // Populate the actual index trees with this candidate's authenticated
        // field values. This is a coherent private candidate, not public adoption.
        for (uint64_t id = 1; id <= 48; ++id) if (id != 3) {
            const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(id + 99)), id);
            tx.stageScalarIndexUpsert(*btree_pages_, hot.index_id, key, id);
        }
        for (const uint64_t id : {1U, 64U}) {
            const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(id == 1 ? "user-000000" : "user-000063"), id);
            tx.stageScalarIndexUpsert(*btree_pages_, cold.index_id, key, id);
        }
        ASSERT_TRUE(tx.commit().committed());
    }
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    const auto fileBytes = io_->fileSize(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    descriptor_.semantic_generation = before.generation;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const auto old = HybridPropertyValueReader::read(context, pin, {NODE, 1}, 2);
        ASSERT_TRUE(old.value); EXPECT_EQ(old.value->value, PropertyValue("user-000000"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(444)}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true;
        struct Counts { int64_t scalar = 0, frames = 0; } counts;
        {
            graph::debug::PerfTrace::ScopedObserver observe(&counts, [](void *p, std::string_view key, int64_t count, bool) noexcept {
                auto &c = *static_cast<Counts *>(p);
                if (key == "native_graph.hybrid.private_scalar_index_images") c.scalar = count;
                if (key == "native_graph.hybrid.private_field_coordinator_frames") c.frames = count;
            });
            try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
            catch (const FormatException &error) {
                EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
                EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
            }
        }
        EXPECT_GT(counts.scalar, 0); EXPECT_GT(counts.frames, 0);
        const std::array<NativeHybridPropertyColumnReplacement, 1> remove{{{{NODE, 1}, 2, std::monostate{}, true}}};
        request.replacements = remove;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const std::runtime_error &error) {
            EXPECT_NE(std::string(error.what()).find("Constraint violation"), std::string::npos) << error.what();
        }
        const auto oldKey = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(100)), 1);
        const auto newKey = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(444)), 1);
        EXPECT_EQ(BTreeIndex(hot.index_id, *btree_pages_, *metadata_).find(pin, oldKey), std::optional<uint64_t>(1));
        EXPECT_FALSE(BTreeIndex(hot.index_id, *btree_pages_, *metadata_).find(pin, newKey));
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        EXPECT_EQ(io_->fileSize(), fileBytes); EXPECT_THROW(context.groupRecord({NODE, 1}), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U);
}
''' +s[a:];p.write_text(s)
