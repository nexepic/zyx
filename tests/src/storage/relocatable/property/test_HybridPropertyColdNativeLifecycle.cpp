#include "storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp"
#include "storage/relocatable/property/HybridPropertyNativeWriteTestAccess.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
using namespace graph::storage;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyColdNativeLifecycleTest : public graph::storage::test::HybridPropertyColdRowRoutingTest {
protected:
    uint64_t cold_index_id_ = 0, unique_id_ = 0;
    void installDurableFixture(bool large = false, bool constrained = false) {
        RecordProperty("fixture_stage", "seed");
        // Lifecycle fixtures obey the existing Catalog/Retire Queue generation
        // contract. Read-only routing fixtures keep epoch99; durable fixtures use
        // initial epoch1 while retaining distinct semantic/block generations.
        physical_generation_ = Generation(1);
        seed(large); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
        RecordProperty("fixture_stage", "catalog");
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
        SystemCatalog catalog(*btree_pages_, *metadata_); uint64_t person = 0;
        {
            auto tx = planner.beginTransaction(); auto schema = catalog.beginWrite(tx);
            ASSERT_EQ(schema.ensurePropertyToken("hot"), 1U); ASSERT_EQ(schema.ensurePropertyToken("cold"), 2U);
            ASSERT_EQ(schema.ensurePropertyToken("unused"), 3U); ASSERT_EQ(schema.ensurePropertyToken("nullable"), 4U);
            const bool nodes = owner_kind_ == NODE;
            if (constrained) {
                ASSERT_TRUE(nodes); ASSERT_EQ(live_owner_count_, 64U);
                person = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
                schema.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_LABEL, person, 0);
                schema.markTokenCardinalityComplete(SystemCatalogSchemaTokenKind::SCSTK_LABEL);
            }
            schema.setGraphCardinalityStatistics({nodes || with_other_owner_kind_ ? live_owner_count_ : 2U,
                !nodes || with_other_owner_kind_ ? live_owner_count_ : 0U});
            if (!nodes || with_other_owner_kind_) for (unsigned type = 1; type <= 7; ++type) {
                const auto token = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "Type" + std::to_string(type));
                ASSERT_EQ(token, type); schema.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE,
                    token, type == 7 ? live_owner_count_ : 0);
            }
            schema.finish(); ASSERT_TRUE(tx.commit().committed());
        }
        if (constrained) {
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
        RecordProperty("fixture_stage", "descriptor");
        {
            auto pin = metadata_->openSnapshot();
            auto frame = HybridPropertyDescriptorStoreTestAccess::read(*metadata_, pin);
            descriptor_.semantic_generation = frame.generation; frame.descriptor = descriptor_;
            // Initial fixture construction only, before any test-observed pin.
            // The existing byte seam installs coherent routing into its already
            // allocated frame. This is NOT a conversion/adoption publication and
            // proves none of the conversion space/data-before-root contracts.
            HybridPropertyDescriptorStoreTestAccess::overwriteForCorruptionTest(*metadata_, pin.superblock(), frame);
        }
        RecordProperty("fixture_stage", "attach");
        HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
        if (!values_) values_ = std::make_unique<LargeValueStore>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *values_, *metadata_);
        RecordProperty("fixture_stage", "ready");
    }
    bool executeLogical(const LogicalGraphMutationBatch &batch) {
        bool committed = false;
        auto callerMemory = std::make_shared<MaterializedByteBudget>(1U << 20);
        {
            graph::utils::OwnedAllocationScope callerScope(callerMemory);
            // Match the backend attachment contract: the sealed Native writer owns
            // pool reuse; source stores are read-only and LargeValue-aware.
            SlottedPagePool pages(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            BTreePageStore trees(pages, *metadata_);
            LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            PropertyCollectionRootStore roots(pages, values, *metadata_);
            EntityPageStore entities(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            EntityLabelSetStore labels(io_, *metadata_, pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            VectorPageStore vectors(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            VectorManifestStore manifests(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
            options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
            options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
            NativeGraphWritePlanner planner(*metadata_, entities, labels, roots, trees, options);
            PageBackedGraphReadAdapter graph(*metadata_, entities, labels, roots, trees); SystemCatalog catalog(trees, *metadata_);
            NativeGraphMutationTokenResolvers tokens;
            tokens.schema_token = [&](const MetadataSnapshotPin &pin, SystemCatalogSchemaTokenKind kind, std::string_view name) {
                return catalog.findSchemaToken(pin, kind, name);
            };
            tokens.property_key_token = [&](const MetadataSnapshotPin &pin, std::string_view name) { return catalog.findPropertyToken(pin, name); };
            tokens.property_key_name = [&](const MetadataSnapshotPin &pin, uint32_t token) { return catalog.findPropertyName(pin, token); };
            NativeGraphMutationExecutor executor(planner, graph, trees, vectors, manifests, *metadata_, tokens,
                [&](const MetadataSnapshotPin &pin) { return catalog.readIndexDefinitions(pin, 64); },
                [&](const MetadataSnapshotPin &pin) { return catalog.readConstraintDefinitions(pin, 64); },
                {64, 64, 128, 64, 128, 128, 64, 128, 4}, &catalog);
            committed = executor.execute(batch).committed();
        }
        EXPECT_EQ(callerMemory->used(), 0U); return committed;
    }
    void verifyColdSetFault(BootstrapPublishFailpoint point) {
        installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
        auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
        const graph::OwnedPropertyValue changed(graph::PropertyValue("fault-id"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 2, changed}}};
        NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1), options),
            BootstrapPublishInterrupted);
        EXPECT_EQ(memory->used(), 0U);
        {
            graph::utils::OwnedAllocationScope scope(memory);
            HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old);
            const auto original = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 2, values_.get()); ASSERT_TRUE(original.value);
            EXPECT_EQ(original.value->value, graph::PropertyValue("user-000000"));
            auto reopened = RelocatableMetadataStore::openReadOnly(io_);
            SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            LargeValueStore values(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            auto recovered = reopened->openSnapshot(); HybridPropertyReadContext context(io_, pages, *reopened, recovered);
            const bool complete = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
            const auto selected = HybridPropertyValueReader::read(context, recovered, {NODE, 1}, 2, &values); ASSERT_TRUE(selected.value);
            EXPECT_EQ(selected.value->value, graph::PropertyValue(complete ? "fault-id" : "user-000000"));
            const auto retained = HybridPropertyValueReader::read(context, recovered, {NODE, 1}, 4, &values); ASSERT_TRUE(retained.value);
            EXPECT_EQ(retained.value->value, graph::PropertyValue(std::string(8193, 'x')));
            EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, recovered, memory, &values).memberships, 51U);
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan());
        }
        EXPECT_EQ(memory->used(), 0U);
    }
    void verifySource() {
        auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
        auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
        {
            graph::utils::OwnedAllocationScope scope(memory);
            HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
            const auto hot = HybridPropertyValueReader::read(context, pin, {owner_kind_, 1}, 1, values_.get());
            ASSERT_TRUE(hot.value); EXPECT_EQ(hot.value->value, graph::PropertyValue(int64_t(100)));
            const auto cold = HybridPropertyValueReader::read(context, pin, {owner_kind_, 1}, 2, values_.get());
            ASSERT_TRUE(cold.value); EXPECT_EQ(cold.value->value, graph::PropertyValue("user-000000"));
            const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory, values_.get());
            EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 51U); EXPECT_EQ(report.columns, 1U);
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
        }
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    }
};
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualDurableNodeRoutingUsesOrdinaryReaderAndCompleteIntegrity) {
    installDurableFixture(); verifySource();
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualDurableLargeColdRoutingUsesOrdinaryReaderAndCompleteIntegrity) {
    installDurableFixture(true); verifySource();
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualDurableEdgeRoutingKeepsTypeCatalogAndCompleteIntegrity) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; installDurableFixture(); verifySource();
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualColdSetPublishesCombinedLargeRootAndPreservesOldPin) {
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const graph::OwnedPropertyValue changed(graph::PropertyValue("changed-id"));
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 2, changed}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    const auto committed = metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1));
    EXPECT_GT(committed.generation, source.generation); EXPECT_EQ(memory->used(), 0U);
    RecordProperty("publication_completed", true);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        HybridPropertyReadContext oldContext(io_, *slotted_pages_, *metadata_, old);
        auto current = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        ASSERT_TRUE(HybridPropertyValueReader::read(oldContext, old, {NODE, 1}, 2, values_.get()).value);
        EXPECT_EQ(HybridPropertyValueReader::read(oldContext, old, {NODE, 1}, 2, values_.get()).value->value, graph::PropertyValue("user-000000"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()).value->value, graph::PropertyValue("changed-id"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 4, values_.get()).value->value, graph::PropertyValue(std::string(8193, 'x')));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(100)));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualIncompatibleHotSetPublishesAllSurvivingDemotedValues) {
    installDurableFixture(); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, false}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    RecordProperty("publication_completed", true);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto current = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.memberships, 51U);
        for (uint64_t id = 1; id <= 48; ++id) {
            const auto field = HybridPropertyValueReader::read(context, current, {NODE, id}, 1, values_.get()); ASSERT_TRUE(field.value);
            EXPECT_EQ(field.value->value, id == 1 ? graph::PropertyValue(false) : id == 3 ? graph::PropertyValue() : graph::PropertyValue(int64_t(id + 99)));
        }
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 2, values_.get()).value->value, graph::PropertyValue("user-000063"));
        HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old);
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {NODE, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(100)));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualOwnerDeletionPublishesEntityFieldsCountsAndSurvivingColdValues) {
    installDurableFixture(); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.owner_deletions = deleted;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    RecordProperty("publication_completed", true);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 1));
        EXPECT_NO_THROW((void)entities_->readNode(old, 1));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.memberships, 48U);
        const auto retained = HybridPropertyValueReader::read(context, current, {NODE, 48}, 1, values_.get()); ASSERT_TRUE(retained.value);
        EXPECT_EQ(retained.value->value, graph::PropertyValue(int64_t(147)));
        SystemCatalog catalog(*btree_pages_, *metadata_); ASSERT_TRUE(catalog.readGraphCardinalityStatistics(current));
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->node_count, 63U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualNativeBirthPublishesLeasedEntityHotNullAndLargeColdRootTogether) {
    live_owner_count_ = 63; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch;
    batch.node_creates.push_back({{}, {{"hot", graph::PropertyValue()}, {"cold", graph::PropertyValue("new-id")},
        {"nullable", graph::PropertyValue(std::string(8193, 'y'))}}});
    ASSERT_TRUE(executeLogical(batch));
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(old, 64)); EXPECT_NO_THROW((void)entities_->readNode(current, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.memberships, 54U);
        const auto hot = HybridPropertyValueReader::read(context, current, {NODE, 64}, 1, values_.get()); ASSERT_TRUE(hot.value);
        EXPECT_EQ(hot.value->value, graph::PropertyValue());
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 2, values_.get()).value->value, graph::PropertyValue("new-id"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 4, values_.get()).value->value, graph::PropertyValue(std::string(8193, 'y')));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()).value->value, graph::PropertyValue("user-000000"));
        SystemCatalog catalog(*btree_pages_, *metadata_); ASSERT_TRUE(catalog.readGraphCardinalityStatistics(current));
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->node_count, 64U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualNativePropertylessEdgeBirthPublishesAdjacencyTypeCountAndLiveness) {
    live_owner_count_ = 63; owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch;
    batch.edge_creates.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2}, "Type7", {}});
    ASSERT_TRUE(executeLogical(batch));
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_FALSE(metadata_->findEdgePageLocationOptional(old, 64)); EXPECT_NO_THROW((void)entities_->readEdge(current, 64));
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get()).memberships, 51U);
        EXPECT_EQ(context.count({owner_kind_, 64}), std::optional<size_t>(0));
        SystemCatalog catalog(*btree_pages_, *metadata_); ASSERT_TRUE(catalog.readGraphCardinalityStatistics(current));
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->edge_count, 64U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ReadOnlyGroupProbesDistinguishPendingAndLiveWithoutColdDecoding) {
    live_owner_count_ = 63; installDurableFixture(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); int64_t decodes = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        graph::debug::PerfTrace::ScopedObserver observe(&decodes, [](void *p, std::string_view key, int64_t n, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += n;
        });
        EXPECT_TRUE(properties_->hasHybridPropertyGroup(pin, {NODE, 1})); EXPECT_TRUE(properties_->isHybridPropertyOwner(pin, {NODE, 1}));
        EXPECT_TRUE(properties_->hasHybridPropertyGroup(pin, {NODE, 64})); EXPECT_FALSE(properties_->isHybridPropertyOwner(pin, {NODE, 64}));
        EXPECT_FALSE(properties_->hasHybridPropertyGroup(pin, {NODE, 65}));
        EXPECT_FALSE(properties_->hasHybridPropertyGroup(pin, {EntityPropertyOwnerKind::EPOK_EDGE, 1}));
    }
    EXPECT_EQ(decodes, 0); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedBirthAndDeletionPublishesOnlyNewEntityAndCompleteColdRoot) {
    live_owner_count_ = 63; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch; batch.node_creates.push_back({{}, {{"hot", graph::PropertyValue("new-incompatible")},
        {"cold", graph::PropertyValue("new-cold")}}});
    for (uint64_t id = 1; id <= 63; ++id) batch.node_deletes.push_back({id, false, 0});
    ASSERT_TRUE(executeLogical(batch)); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        for (uint64_t id = 1; id <= 63; ++id) { EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, id)); EXPECT_NO_THROW((void)entities_->readNode(old, id)); }
        EXPECT_NO_THROW((void)entities_->readNode(current, 64)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(old, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.memberships, 2U); EXPECT_EQ(report.cold_rows, 1U);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 1, values_.get()).value->value, graph::PropertyValue("new-incompatible"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 2, values_.get()).value->value, graph::PropertyValue("new-cold"));
        SystemCatalog catalog(*btree_pages_, *metadata_); EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->node_count, 1U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualFinalGroupDeletionRemovesAllEntityAndFieldAuthorityTogether) {
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); std::array<EntityPropertyKey, 64> deleted{};
    for (unsigned i = 0; i < 64; ++i) deleted[i] = {NODE, i + 1U};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.owner_deletions = deleted;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        for (uint64_t id = 1; id <= 64; ++id) { EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, id)); EXPECT_NO_THROW((void)entities_->readNode(old, id)); }
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current); EXPECT_FALSE(context.hasNativeGroups());
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get()).groups, 0U);
        SystemCatalog catalog(*btree_pages_, *metadata_); EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->node_count, 0U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualInversePublishesCompleteLargeRowsAndPreservesOldColdSnapshot) {
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    NativeGraphHybridPropertyRootMutation request{source.database_id, source.hybrid_property_descriptor_root,
        NativeHybridPropertyRootMode::NHPRM_REPLACE, {}, NativeGraphHybridPropertyConversion{NODE, 1, memory, nullptr,
            PhysicalSlotId(40), PhysicalSlotId(80), 902}};
    request.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    request.conversion->publish_candidate = true; NativeGraphDerivedStateMutation derived;
    static_cast<void>(metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived,
        LogSequenceNumber(source.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {},
        [](auto &) {}, {}, {}, {}, {}, {}, {}, {}, source.generation, {}, {}, {}, {}, {}, request));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_FALSE(properties_->isHybridPropertyOwner(current, {NODE, 1})); EXPECT_TRUE(properties_->isHybridPropertyOwner(old, {NODE, 1}));
        const auto row = properties_->readEntity(current, {NODE, 1}); ASSERT_EQ(row.entries.size(), 3U);
        EXPECT_EQ(row.entries[0].value, graph::PropertyValue(int64_t(100))); EXPECT_EQ(row.entries[1].value, graph::PropertyValue("user-000000"));
        EXPECT_EQ(row.entries[2].value, graph::PropertyValue(std::string(8193, 'x')));
        EXPECT_TRUE(properties_->readEntityPropertyOptional(current, {NODE, 3}, 1)->value.getType() == graph::PropertyType::NULL_TYPE);
        EXPECT_FALSE(properties_->readEntityOptional(current, {NODE, 49})); EXPECT_NO_THROW((void)entities_->readNode(current, 49));
        HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old);
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()).value->value, graph::PropertyValue(std::string(8193, 'x')));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, PartialInactiveRootFaultReopensOnlyOriginalColdValue) {
    verifyColdSetFault(BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, CompleteInactiveRootFaultReopensCompleteColdValueAndManifest) {
    verifyColdSetFault(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, CancellationAfterPrivateWritePreservesOldRootRefundsAndAllowsRetry) {
    struct Cancelled final : std::runtime_error { Cancelled() : std::runtime_error("cancel cold private write") {} };
    struct Cancel final : CancellationCheckpoint {
        StorageIO &io; uint64_t epoch; bool armed = true;
        explicit Cancel(StorageIO &source) : io(source), epoch(source.dataMutationEpoch()) {}
        void check() const override { if (armed && io.dataMutationEpoch() != epoch) throw Cancelled(); }
    };
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); Cancel cancellation(*io_);
    const graph::OwnedPropertyValue changed(graph::PropertyValue("retry-id"));
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 2, changed}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.cancellation = &cancellation;
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)), Cancelled);
    EXPECT_NE(io_->dataMutationEpoch(), cancellation.epoch); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), source); cancellation.armed = false;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old), context(io_, *slotted_pages_, *metadata_, current);
        const auto original = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 2, values_.get()); ASSERT_TRUE(original.value);
        const auto final = HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()); ASSERT_TRUE(final.value);
        EXPECT_EQ(original.value->value, graph::PropertyValue("user-000000")); EXPECT_EQ(final.value->value, graph::PropertyValue("retry-id"));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualColdIndexesAndUniqueRequiredConstraintsPublishTogetherOrRefuse) {
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
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualPropertyClearKeepsLiveOwnerAndRemovesAllItsHotAndColdFields) {
    installDurableFixture(true); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch;
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {}});
    ASSERT_TRUE(executeLogical(batch)); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_NO_THROW((void)entities_->readNode(current, 1)); EXPECT_TRUE(properties_->isHybridPropertyOwner(current, {NODE, 1}));
        EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(current, {NODE, 1}));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        EXPECT_EQ(context.count({NODE, 1}), std::optional<size_t>(0));
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.memberships, 48U);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 48}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(147)));
        HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old);
        const auto oldLarge = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()); ASSERT_TRUE(oldLarge.value);
        EXPECT_EQ(oldLarge.value->value, graph::PropertyValue(std::string(8193, 'x')));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualInverseRetainsOtherNamespaceOnTheSameLogicalFieldFrame) {
    with_other_owner_kind_ = true; installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        HybridPropertyReadContext before(io_, *slotted_pages_, *metadata_, old);
        const auto report = HybridPropertyDirectoryIntegrity::verify(before, old, memory, values_.get());
        EXPECT_EQ(report.groups, 2U); EXPECT_EQ(report.memberships, 102U);
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(before, old, field_page_id_, memory, values_.get()), 8U);
    }
    NativeGraphHybridPropertyRootMutation request{source.database_id, source.hybrid_property_descriptor_root,
        NativeHybridPropertyRootMode::NHPRM_REPLACE, {}, NativeGraphHybridPropertyConversion{NODE, 1, memory, nullptr,
            PhysicalSlotId(40), PhysicalSlotId(80), 902}};
    request.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    request.conversion->publish_candidate = true; NativeGraphDerivedStateMutation derived;
    static_cast<void>(metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived,
        LogSequenceNumber(source.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {},
        [](auto &) {}, {}, {}, {}, {}, {}, {}, {}, source.generation, {}, {}, {}, {}, {}, request));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        const auto edgeKind = EntityPropertyOwnerKind::EPOK_EDGE;
        EXPECT_FALSE(properties_->isHybridPropertyOwner(current, {NODE, 1})); EXPECT_TRUE(properties_->isHybridPropertyOwner(current, {edgeKind, 1}));
        EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 1}, 1)->value, graph::PropertyValue(int64_t(100)));
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 51U); EXPECT_EQ(report.columns, 1U);
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, current, field_page_id_, memory, values_.get()), 4U);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {edgeKind, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(200)));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {edgeKind, 1}, 2, values_.get()).value->value, graph::PropertyValue("other-first"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {edgeKind, 64}, 2, values_.get()).value->value, graph::PropertyValue("other-last"));
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()).value->value, graph::PropertyValue(std::string(8193, 'x')));
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {edgeKind, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(200)));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualColdValueReclamationPreservesLiveValueAndPinnedOldCarrierThenReusesFrames) {
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    const auto originalManifest = values_->manifest(old, value_reference_); ASSERT_FALSE(originalManifest.chunks.empty());
    const auto oldData = metadata_->findPageLocation(old, originalManifest.chunks.front().first_data_page_id);
    const auto originalDataPool = metadata_->findCatalog(old, oldData.segment_id);
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
        SharedPagePoolAllocator pools(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        uint8_t freeBefore = 0;
        { auto pin = metadata_->openSnapshot(); freeBefore = pools.availableFrameCount(metadata_->findCatalog(pin, oldData.segment_id)); }
        const auto nextLsn = [&] { return LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1); };
        RecordProperty("gc_stage", "retire");
        const auto retired = reclaimer.reclaim(options, nextLsn());
        RecordProperty("gc_stage", "retired");
        EXPECT_EQ(retired.reachable_values, 1U); EXPECT_EQ(retired.retired_values, 1U);
        {
            auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findLargeValueManifestOptional(current, value_reference_.value_id));
            EXPECT_TRUE(metadata_->isPhysicalSlotAllocated(current, originalDataPool.physical_slot));
            const auto pool = metadata_->findCatalog(current, oldData.segment_id);
            if (pool.state == SegmentState::SGS_SEALED) EXPECT_EQ(pools.availableFrameCount(pool), freeBefore);
            HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old), context(io_, *slotted_pages_, *metadata_, current);
            const auto original = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()); ASSERT_TRUE(original.value);
            EXPECT_EQ(original.value->value, graph::PropertyValue(std::string(8193, 'x')));
            const auto live = HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()); ASSERT_TRUE(live.value);
            EXPECT_EQ(live.value->value, graph::PropertyValue("gc-id"));
        }
        old.release(); RecordProperty("gc_stage", "release");
        const auto released = reclaimer.reclaim(options, nextLsn()); EXPECT_EQ(released.retired_values, 0U);
        RecordProperty("gc_stage", "released"); auto current = metadata_->openSnapshot();
        // Fully released obsolete pools can also be purged from Catalog. A
        // surviving shared pool instead proves reclaimed capacity by its public
        // free-frame count; no private allocator/header seam is needed.
        if (metadata_->isPhysicalSlotAllocated(current, originalDataPool.physical_slot)) {
            const auto pool = metadata_->findCatalog(current, oldData.segment_id);
            EXPECT_EQ(pool.state, SegmentState::SGS_SEALED); EXPECT_GT(pools.availableFrameCount(pool), freeBefore);
        }
        RecordProperty("gc_stage", "checked");
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualSharedColdFieldRelocationPreservesBothKindsAndReleasesOnlyAfterOldPin) {
    with_other_owner_kind_ = true; installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    const auto location = metadata_->findPageLocation(old, field_page_id_); const auto pool = metadata_->findCatalog(old, location.segment_id);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    NativeHybridPropertyPoolRelocation move{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        pool.segment_id, PhysicalSlotId(80), PhysicalSlotId(80), memory};
    static_cast<void>(metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        const auto moved = metadata_->findPageLocation(current, field_page_id_);
        EXPECT_EQ(moved.page_id, location.page_id); EXPECT_NE(moved.segment_id, location.segment_id);
        EXPECT_EQ(metadata_->findPageLocation(old, field_page_id_), location);
        EXPECT_EQ(metadata_->findCatalog(current, pool.segment_id).state, SegmentState::SGS_RETIRED);
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get()).memberships, 102U);
        const auto now = HybridPropertyValueReader::read(context, current, {NODE, 1}, 4, values_.get()); ASSERT_TRUE(now.value);
        const auto then = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()); ASSERT_TRUE(then.value);
        EXPECT_EQ(now.value->value, graph::PropertyValue(std::string(8193, 'x'))); EXPECT_EQ(then.value->value, now.value->value);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, values_.get()).value->value,
            graph::PropertyValue("other-first"));
    }
    const auto nextLsn = [&] { return LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1); };
    static_cast<void>(metadata_->reclaimRetiredPhysicalSlots(nextLsn()));
    { auto current = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->isPhysicalSlotAllocated(current, pool.physical_slot)); }
    old.release(); static_cast<void>(metadata_->reclaimRetiredPhysicalSlots(nextLsn()));
    { auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(current, pool.physical_slot)); }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedNamespaceColdSetSharesOnePrivateFrameAndPreservesOldPins) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot();
    const auto source = old.superblock(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 2, graph::OwnedPropertyValue(graph::PropertyValue("node-new"))},
        {{EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, graph::OwnedPropertyValue(graph::PropertyValue("edge-new"))}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            const auto read = [&](HybridPropertyReadContext &reader, const MetadataSnapshotPin &pin, uint64_t id, uint32_t token) {
                auto field = HybridPropertyValueReader::read(reader, pin, {kind, id}, token, values_.get());
                EXPECT_TRUE(field.value); return field.value ? field.value->value : graph::PropertyValue();
            };
            EXPECT_EQ(read(context, current, 1, 2), graph::PropertyValue(kind == NODE ? "node-new" : "edge-new"));
            EXPECT_EQ(read(previous, old, 1, 2), graph::PropertyValue(kind == NODE ? "user-000000" : "other-first"));
            EXPECT_EQ(read(context, current, 64, 2), graph::PropertyValue(kind == NODE ? "user-000063" : "other-last"));
            EXPECT_EQ(read(context, current, 1, 1), graph::PropertyValue(int64_t(kind == NODE ? 100 : 200)));
        }
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.groups, 2U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedHotAndColdSetMergesCompatibleBaseBeforeColdRepacking) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot();
    const auto source = old.superblock(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 1, int64_t(999)},
        {{EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, graph::OwnedPropertyValue(graph::PropertyValue("edge-new"))}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(999)));
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {NODE, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(100)));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, values_.get()).value->value, graph::PropertyValue("edge-new"));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedDemotionAndColdSetMigratesOnlyOneGroupsOwners) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 1, false}, {{EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, graph::OwnedPropertyValue(graph::PropertyValue("new-edge"))}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        for (uint64_t id = 1; id <= 48; ++id) {
            const auto field = HybridPropertyValueReader::read(context, current, {NODE, id}, 1, values_.get()); ASSERT_TRUE(field.value);
            EXPECT_EQ(field.value->value, id == 1 ? graph::PropertyValue(false) : id == 3 ? graph::PropertyValue() : graph::PropertyValue(int64_t(id + 99)));
        }
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, values_.get()).value->value, graph::PropertyValue("new-edge"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 48}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(247)));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedDemotionsCannotDoubleTheOriginal64OwnerLimit) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, false}, {{EntityPropertyOwnerKind::EPOK_EDGE, 1}, 1, false}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), source); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualLogicalMixedCollectionsUseTheSameAtomicColdCoordinator) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch;
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {{"hot", graph::PropertyValue(int64_t(1010))},
        {"cold", graph::PropertyValue("logical-node")}, {"nullable", graph::PropertyValue()}}});
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_EDGE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {{"hot", graph::PropertyValue(int64_t(2020))}, {"cold", graph::PropertyValue("logical-edge")}}});
    ASSERT_TRUE(executeLogical(batch)); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()).value->value, graph::PropertyValue("logical-node"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, values_.get()).value->value, graph::PropertyValue("logical-edge"));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyColdNativeLifecycleTest, ActualLogicalMixedDeletionRemovesEachNamespacesColdMembership) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch; batch.node_deletes.push_back({64, false, 0}); batch.edge_deletes.push_back({64});
    ASSERT_TRUE(executeLogical(batch)); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 64)); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(current, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            EXPECT_FALSE(context.ownerLiveness({kind, 64}).value()); EXPECT_TRUE(previous.ownerLiveness({kind, 64}).value());
            EXPECT_TRUE(HybridPropertyValueReader::read(context, current, {kind, 1}, 2, values_.get()).value);
        }
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualLogicalMixedBirthsKeepIndependentPendingMasksAndSealedLeases) {
    with_other_owner_kind_ = true; live_owner_count_ = 63; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch;
    batch.node_creates.push_back({{}, {{"hot", graph::PropertyValue()}, {"cold", graph::PropertyValue("born-node")}}});
    batch.edge_creates.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2}, "Type7",
        {{"hot", graph::PropertyValue()}, {"cold", graph::PropertyValue("born-edge")}}});
    ASSERT_TRUE(executeLogical(batch)); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_NO_THROW((void)entities_->readNode(current, 64));
        EXPECT_NO_THROW((void)entities_->readEdge(current, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            EXPECT_TRUE(context.ownerLiveness({kind, 64}).value()); EXPECT_FALSE(previous.ownerLiveness({kind, 64}).value());
            EXPECT_EQ(HybridPropertyValueReader::read(context, current, {kind, 64}, 2, values_.get()).value->value,
                graph::PropertyValue(kind == NODE ? "born-node" : "born-edge"));
        }
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
