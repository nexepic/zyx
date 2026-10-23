from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();s=s.replace('#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"','#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"\n#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"\n#include "graph/storage/relocatable/index/BTreeIndex.hpp"\n#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"',1)
s=s.replace('''    void installDurableFixture(bool large = false) {''','''    uint64_t cold_index_id_ = 0, unique_id_ = 0;
    void installDurableFixture(bool large = false, bool constrained = false) {''',1)
s=s.replace('''        SystemCatalog catalog(*btree_pages_, *metadata_);
        {
            auto tx''','''        SystemCatalog catalog(*btree_pages_, *metadata_); uint64_t person = 0;
        {
            auto tx''',1)
s=s.replace('''            const bool nodes = owner_kind_ == NODE;
            schema.setGraphCardinalityStatistics''','''            const bool nodes = owner_kind_ == NODE;
            if (constrained) {
                ASSERT_TRUE(nodes); ASSERT_EQ(live_owner_count_, 64U);
                person = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
                schema.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_LABEL, person, 0);
                schema.markTokenCardinalityComplete(SystemCatalogSchemaTokenKind::SCSTK_LABEL);
            }
            schema.setGraphCardinalityStatistics''',1)
old='''        RecordProperty("fixture_stage", "descriptor");''';new='''        if (constrained) {
            {
                auto tx = planner.beginTransaction(); const std::array labels{person};
                tx.replaceNodeLabels(1, labels); tx.replaceNodeLabels(64, labels); ASSERT_TRUE(tx.commit().committed());
            }
            {
                auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
                IndexDefinition index; index.name = "cold_values"; index.kind = IndexDefinitionKind::IDK_PROPERTY;
                index.label_token_id = person; index.property_names = {"cold"}; index = schema.createIndexDefinition(index);
                cold_index_id_ = index.index_id;
                ConstraintDefinition unique; unique.name = "unique_cold"; unique.kind = ConstraintDefinitionKind::CDK_UNIQUE;
                unique.schema_token_id = person; unique.property_names = {"cold"}; unique = schema.createConstraintDefinition(unique);
                unique_id_ = unique.constraint_id;
                ConstraintDefinition required; required.name = "required_cold"; required.kind = ConstraintDefinitionKind::CDK_NOT_NULL;
                required.schema_token_id = person; required.property_names = {"cold"}; static_cast<void>(schema.createConstraintDefinition(required));
                schema.finish(); std::vector<BTreePageEntry> scalar, support;
                for (const uint64_t id : {1U, 64U}) {
                    const graph::PropertyValue value(id == 1 ? "user-000000" : "user-000063");
                    const auto key = IndexKeyCodec::encodeEntryKey(value, id);
                    scalar.push_back({graph::utils::OwnedStdVector<std::byte>(key.begin(), key.end()), id});
                    const std::array<graph::PropertyValue, 1> values{value};
                    const auto canonical = ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), id);
                    support.push_back({graph::utils::OwnedStdVector<std::byte>(canonical.begin(), canonical.end()), id});
                }
                std::ranges::sort(scalar, {}, &BTreePageEntry::key); std::ranges::sort(support, {}, &BTreePageEntry::key);
                tx.stageScalarIndexCreate(*btree_pages_, cold_index_id_, scalar);
                tx.stageScalarIndexCreate(*btree_pages_, unique_id_, support); ASSERT_TRUE(tx.commit().committed());
            }
        }
        RecordProperty("fixture_stage", "descriptor");''';assert old in s;s=s.replace(old,new,1)
at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdNativeLifecycleTest, ActualColdIndexesAndUniqueRequiredConstraintsPublishTogetherOrRefuse) {
    installDurableFixture(false, true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const graph::OwnedPropertyValue changed(graph::PropertyValue("fresh-id"));
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 2, changed}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    auto current = metadata_->openSnapshot();
    const auto supportKey = [](std::string_view text) {
        const std::array<graph::PropertyValue, 1> values{graph::PropertyValue(std::string(text))};
        return ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), 1);
    };
    EXPECT_EQ(BTreeIndex(cold_index_id_, *btree_pages_, *metadata_).find(old,
        IndexKeyCodec::encodeEntryKey(graph::PropertyValue("user-000000"), 1)), std::optional<uint64_t>(1));
    EXPECT_FALSE(BTreeIndex(cold_index_id_, *btree_pages_, *metadata_).find(current,
        IndexKeyCodec::encodeEntryKey(graph::PropertyValue("user-000000"), 1)));
    EXPECT_EQ(BTreeIndex(cold_index_id_, *btree_pages_, *metadata_).find(current,
        IndexKeyCodec::encodeEntryKey(graph::PropertyValue("fresh-id"), 1)), std::optional<uint64_t>(1));
    EXPECT_EQ(BTreeIndex(unique_id_, *btree_pages_, *metadata_).find(old, supportKey("user-000000")), std::optional<uint64_t>(1));
    EXPECT_FALSE(BTreeIndex(unique_id_, *btree_pages_, *metadata_).find(current, supportKey("user-000000")));
    EXPECT_EQ(BTreeIndex(unique_id_, *btree_pages_, *metadata_).find(current, supportKey("fresh-id")), std::optional<uint64_t>(1));
    const auto committed = current.superblock(); const auto epoch = io_->dataMutationEpoch();
    request.expected_generation = committed.generation; request.expected_descriptor = committed.hybrid_property_descriptor_root;
    const graph::OwnedPropertyValue duplicate(graph::PropertyValue("user-000063"));
    const std::array<NativeHybridPropertyColumnReplacement, 1> duplicates{{{{NODE, 1}, 2, duplicate}}}; request.replacements = duplicates;
    try { static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(committed.checkpoint_lsn.value() + 1))); FAIL(); }
    catch (const std::runtime_error &error) { EXPECT_NE(std::string(error.what()).find("Constraint violation: 'unique_cold'"), std::string::npos) << error.what(); }
    const std::array<NativeHybridPropertyColumnReplacement, 1> erase{{{{NODE, 1}, 2, std::monostate{}, true}}}; request.replacements = erase;
    try { static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(committed.checkpoint_lsn.value() + 1))); FAIL(); }
    catch (const std::runtime_error &error) { EXPECT_NE(std::string(error.what()).find("Constraint violation: 'required_cold'"), std::string::npos) << error.what(); }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), committed); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[at:];p.write_text(s)
