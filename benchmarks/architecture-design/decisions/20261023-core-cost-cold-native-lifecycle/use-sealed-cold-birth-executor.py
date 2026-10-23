from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();s=s.replace('#include "graph/storage/relocatable/schema/SystemCatalog.hpp"','#include "graph/storage/relocatable/schema/SystemCatalog.hpp"\n#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"\n#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"',1)
old='''    void verifySource() {''';new='''    void executeLogical(const LogicalGraphMutationBatch &batch) {
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
        ASSERT_TRUE(executor.execute(batch).committed());
    }
    void verifySource() {''';assert old in s;s=s.replace(old,new,1)
a=s.index('    NativeGraphWritePlannerOptions options;',s.index('TEST_F(HybridPropertyColdNativeLifecycleTest, ActualNativeBirth'));b=s.index('    auto memory =',a)
s=s[:a]+'''    LogicalGraphMutationBatch batch;
    batch.node_creates.push_back({{}, {{"hot", graph::PropertyValue()}, {"cold", graph::PropertyValue("new-id")},
        {"nullable", graph::PropertyValue(std::string(8193, 'y'))}}});
    executeLogical(batch);
'''+s[b:]
a=s.index('    NativeGraphWritePlannerOptions options;',s.index('TEST_F(HybridPropertyColdNativeLifecycleTest, ActualNativePropertylessEdgeBirth'));b=s.index('    auto memory =',a)
s=s[:a]+'''    LogicalGraphMutationBatch batch;
    batch.edge_creates.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2}, "Type7", {}});
    executeLogical(batch);
'''+s[b:];p.write_text(s)
