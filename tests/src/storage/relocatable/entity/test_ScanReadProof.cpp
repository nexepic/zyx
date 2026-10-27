#include "BuildingScanMembershipFixture.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/LogicalGraphNodeScan.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/CertifiedScanMembership.hpp"

static_assert(!std::is_default_constructible_v<VerifiedScanRoot>);
static_assert(!std::is_default_constructible_v<ScanBatchReadGrant>);
static_assert(!std::is_copy_constructible_v<ScanBatchReadGrant>);
static_assert(!std::is_default_constructible_v<ScanBatchReadScope>);
static_assert(!std::is_convertible_v<ScanBlockMembership, VerifiedScanRoot>);

#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
class ScanReadProofTest : public BuildingScanMembershipTest {
protected:
    std::filesystem::path target;
    std::shared_ptr<graph::query::execution::MaterializedByteBudget> source =
        std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    std::shared_ptr<graph::query::execution::MaterializedByteBudget> work =
        std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    std::shared_ptr<graph::query::execution::MaterializedByteBudget> control =
        std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    std::unique_ptr<ScanBuildingImageBuilder> builder;
    void makeImage(bool complete = true, bool scalarRows = false, bool oversizedLabels = false, size_t typedColumns = 2U, bool terminalNode = false, size_t nodeCount = 64U, bool seedRowEdge = false) {
        if (scalarRows) {
            initialize(true, true);
            LargeValueStore values(io, *metadata);
            PropertyCollectionRootStore properties(*pages, values, *metadata);
            BTreePageStore indexes(*pages, *metadata);
            NativeGraphWritePlannerOptions options;
            options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
            options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
            if (terminalNode) options.maximum_logical_id_reservation = (std::numeric_limits<size_t>::max)() - 1U;
            NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
            auto tx = planner.beginTransaction();
            SystemCatalog catalog(indexes, *metadata);
            auto schema = catalog.beginWrite(tx);
            ASSERT_EQ(schema.ensurePropertyToken("number"), 1U);
            ASSERT_EQ(schema.ensurePropertyToken("text"), 2U);
            ASSERT_EQ(schema.ensurePropertyToken("list"), 3U);
            for (size_t token = 4; token <= typedColumns; ++token)
                ASSERT_EQ(schema.ensurePropertyToken("K" + std::to_string(token)), token);
            const auto label = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Nodes");
            for (size_t i = 0; i < nodeCount; ++i) {
                std::array<uint64_t, 2> nodeLabels{label, 0};
                size_t labelCount = 1;
                if (oversizedLabels) {
                    nodeLabels[0] = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL,
                        "L" + std::to_string(i + 1U));
                    nodeLabels[1] = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL,
                        "L" + std::to_string(i + 65U));
                    labelCount = 2;
                    schema.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_LABEL, nodeLabels[0], 1U);
                    schema.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_LABEL, nodeLabels[1], 1U);
                }
                const auto id = tx.createNode(std::span(nodeLabels).first(labelCount));
                PropertyCollectionRoot row{id, graph::Node::typeId,
                    {{1, graph::PropertyValue(int64_t(id))}, {2, graph::PropertyValue("short")}}};
                if (typedColumns > 2U) {
                    row.entries.clear();
                    for (size_t token = 1; token <= typedColumns; ++token)
                        row.entries.push_back({uint32_t(token), graph::PropertyValue(int64_t(id + token - 1U))});
                } else if (i == 0U) row.entries.push_back({3, graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})})});
                tx.replaceNodeProperty(std::move(row));
            }
            if (seedRowEdge) {
                const auto weight = schema.ensurePropertyToken("weight");
                const auto link = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "LINK");
                const auto edge = tx.createEdge(1, 2, link);
                tx.replaceEdgeProperty({edge, graph::Edge::typeId, {{weight, graph::PropertyValue(int64_t{0})}}});
                schema.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, link, 1U);
            }
            schema.setGraphCardinalityStatistics({uint64_t(nodeCount), seedRowEdge ? 1U : 0U});
            if (!oversizedLabels) schema.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_LABEL, label, uint64_t(nodeCount));
            schema.finish();
            ASSERT_TRUE(tx.commit().committed());
            if (terminalNode) {
                auto terminal = planner.beginTransaction();
                terminal.reserveNodeIds(UINT64_MAX - uint64_t(nodeCount));
                ASSERT_EQ(terminal.createNodeWithReservedId(UINT64_MAX, {}), UINT64_MAX);
                auto stats = catalog.beginWrite(terminal);
                stats.setGraphCardinalityStatistics({uint64_t(nodeCount + 1U), 0U}); stats.finish();
                ASSERT_TRUE(terminal.commit().committed());
            }
        } else { insert(1, 0, {7}); insert(65, 1, {11}); }
        static_cast<void>(makeLegacyRowSource());
        std::filesystem::resize_file(path, std::filesystem::file_size(path) + 16U * RSS_SEGMENT_SIZE);
        target = path.string() + ".scan";
        builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, source,
            UINT64_C(64) * 1024U * 1024U, 989);
        const auto converted = builder->constructFields(nullptr, work);
        if (scalarRows) ASSERT_EQ(converted.converted_groups, (nodeCount + 63U) / 64U);
        if (!complete) return;
        bool stopped = false;
        for (size_t step = 0; step < 128U; ++step) {
            const auto release = builder->releaseRetiredPoolOnce(work);
            ASSERT_FALSE(release.blocked);
            if (release.released) continue;
            const auto before = builder->buildingRoot();
            if (builder->reclaimMetadataOnce(work).superblock != before) continue;
            if (builder->compactDataPoolOnce(work)) continue;
            if (builder->compactMetadataPoolOnce(work)) continue;
            stopped = true; break;
        }
        ASSERT_TRUE(stopped);
        static_cast<void>(builder->truncateReleasedTailOnce(work));
        ASSERT_LE(builder->targetFileBytes() * 10U, builder->sourceFileBytes() * 9U);
        static_cast<void>(builder->complete(work));
    }
    std::shared_ptr<StorageIO> targetIo() { return ScanBuildingImageBuilderTestAccess::io(*builder); }
    RelocatableMetadataStore &store() { return ScanBuildingImageBuilderTestAccess::metadata(*builder); }
    HybridPropertyDescriptor targetDescriptor(const MetadataSnapshotPin &pin) {
        MetadataPageFile pages(targetIo(), pin.superblock());
        return pages.readHybridPropertyDescriptor(pin.superblock().hybrid_property_descriptor_root, pin.superblock()).descriptor;
    }
    NativeGraphMutationExecutionResult mutateImage(const LogicalGraphMutationBatch &batch) {
        SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore values(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore properties(pages, values, store());
        BTreePageStore trees(pages, store());
        EntityPageStore entities(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityLabelSetStore labels(targetIo(), store(), pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PageBackedGraphReadAdapter graph(store(), entities, labels, properties, trees);
        VectorPageStore vectors(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        VectorManifestStore manifests(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(1); options.last_slot = PhysicalSlotId(127);
        options.property_roots = {PhysicalSlotId(1), PhysicalSlotId(127), 989};
        options.logical_id_lease_block_size = 64;
        NativeGraphWritePlanner writer(store(), entities, labels, properties, trees, options);
        SystemCatalog catalog(trees, store());
        NativeGraphMutationExecutor executor(writer, graph, trees, vectors, manifests, store(), {},
            [](const auto &) { return graph::utils::OwnedStdVector<IndexDefinition>{}; },
            [](const auto &) { return graph::utils::OwnedStdVector<ConstraintDefinition>{}; },
            NativeGraphMutationExecutorOptions{64, 64, 4096, 64, 4096, 4096, 64, 128, 4096}, &catalog);
        return executor.execute(batch);
    }
    void checkDeletionPublicationFailure(BootstrapPublishFailpoint failpoint, bool published) {
        ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
        auto caller = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
        graph::utils::OwnedAllocationScope owner(caller);
        auto old = store().openSnapshot(); const auto before = old.superblock();
        const std::array<EntityPropertyKey, 1> erased{{{EntityPropertyOwnerKind::EPOK_NODE, 1}}};
        auto writeMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        NativeHybridPropertyColumnWrite change{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, writeMemory, PhysicalSlotId(1), PhysicalSlotId(127), 989, nullptr, true, erased};
        MetadataPublishOptions options; options.bootstrap_failpoint = failpoint;
        EXPECT_THROW(static_cast<void>(store().mutateHybridPropertyColumns(change,
            LogSequenceNumber(before.checkpoint_lsn.value() + 1U), options)), BootstrapPublishInterrupted);
        EXPECT_EQ(writeMemory->used(), 0U);
        auto recovered = RelocatableMetadataStore::openReadOnly(targetIo()); auto pin = recovered->openSnapshot();
        const auto desc = targetDescriptor(pin);
        EXPECT_EQ(desc.scan_live_node_count, published ? 63U : 64U);
        EXPECT_EQ(recovered->findNodePageLocationOptional(pin, 1).has_value(), !published);
        if (!published) EXPECT_EQ(pin.superblock(), before);
        EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), *recovered, work).scan()));
        SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore values(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore properties(pages, values, store());
        const auto retained = properties.readEntityOptional(old, {EntityPropertyOwnerKind::EPOK_NODE, 1});
        ASSERT_TRUE(retained); EXPECT_EQ(retained->entries.size(), 3U);
    }
    void TearDown() override {
        builder.reset();
        EXPECT_EQ(source->used(), 0U); EXPECT_EQ(work->used(), 0U); EXPECT_EQ(control->used(), 0U);
        std::error_code ignored; std::filesystem::remove(target, ignored);
        BuildingScanMembershipTest::TearDown();
    }
};

TEST_F(ScanReadProofTest, PublicReadOnlyBackendStreamsPersistentMembershipAndHonorsPrefixStop) {
    ASSERT_NO_FATAL_FAILURE(makeImage());
    RelocatableStorageBackendOptions options;
    options.data_path = target;
    RelocatableStorageBackend backend(options);
    ASSERT_NO_THROW(backend.open());
    {
        auto snapshot = backend.openLogicalGraphReadSnapshot();
        auto *provider = dynamic_cast<LogicalGraphNodeScanProvider *>(snapshot.get());
        ASSERT_NE(provider, nullptr);
        const std::array<std::string_view, 1> keys{"missing"};
        size_t storedBatches = 0;
        graph::debug::PerfTrace::ScopedObserver observer(&storedBatches,
            [](void *p, std::string_view key, int64_t value, bool duration) noexcept {
                if (!duration && key == "native_graph.read.stored_membership_scan_batches")
                    *static_cast<size_t *>(p) += size_t(value);
            });
        std::vector<uint64_t> ids;
        size_t examined = 0;
        auto result = provider->tryVisitNodeScanBatches(SIZE_MAX, 1, {}, keys,
            [&](auto batch, size_t count, const auto &) {
                ids.insert(ids.end(), batch.begin(), batch.end()); examined += count; return true;
            }, [](size_t) {});
        ASSERT_TRUE(result.has_value()); EXPECT_TRUE(*result);
        EXPECT_EQ(ids, (std::vector<uint64_t>{1, 65})); EXPECT_EQ(examined, 2U);
        EXPECT_EQ(storedBatches, 2U);
        size_t calls = 0;
        result = provider->tryVisitNodeScanBatches(SIZE_MAX, 1, {}, keys,
            [&](auto, size_t, const auto &) { ++calls; return false; }, [](size_t) {});
        ASSERT_TRUE(result.has_value()); EXPECT_FALSE(*result); EXPECT_EQ(calls, 1U);
    }
    EXPECT_NO_THROW(backend.close());
    builder.reset();
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend writable(options);
    EXPECT_NO_THROW(writable.open());
    EXPECT_NO_THROW(writable.close());
}

TEST_F(ScanReadProofTest, PublicWritableLifecycleFallsBackBeforeOutputAndReopensWithFreshMembershipProof) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); ASSERT_NO_THROW(backend.open());
    auto old = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(old->readNodeProperty(1, "number"), graph::PropertyValue(int64_t{1}));
    LogicalGraphMutationBatch batch;
    batch.node_creates.push_back({{"Nodes"}, {{"number", graph::PropertyValue(int64_t{909})}}});
    batch.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {"Changed"}});
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {{"number", graph::PropertyValue(int64_t{999})}}});
    const auto added = backend.executeLogicalGraphMutationBatch(batch);
    ASSERT_EQ(added.created_node_ids.size(), 1U);
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(old->readNodeProperty(1, "number"), graph::PropertyValue(int64_t{1}));
    EXPECT_EQ(current->readNodeProperty(1, "number"), graph::PropertyValue(int64_t{999}));
    const std::array<std::string_view, 1> keys{"number"};
    for (const bool prior : {true, false}) {
        auto *provider = dynamic_cast<LogicalGraphNodeScanProvider *>((prior ? old : current).get());
        ASSERT_NE(provider, nullptr); size_t count = 0;
        const auto result = provider->tryVisitNodeScanBatches(SIZE_MAX, 512, {}, keys,
            [&](auto ids, size_t, const auto &) { count += ids.size(); return true; }, [](size_t) {});
        ASSERT_TRUE(result.has_value()); EXPECT_TRUE(*result); EXPECT_EQ(count, prior ? 64U : 65U);
    }
    old.reset(); current.reset(); EXPECT_NO_THROW(backend.close());
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    auto reopened = std::make_shared<RelocatableStorageBackend>(options); ASSERT_NO_THROW(reopened->open());
    graph::query::QueryEngine engine(reopened);
    auto result = engine.execute("MATCH (n:Changed) RETURN sum(n.number) AS total");
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), graph::PropertyValue(int64_t{999}));
}

TEST_F(ScanReadProofTest, PublicPropertyTypeChangesAndLargeValuesPreservePinnedAndReopenedReads) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true)); builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto old = backend.openLogicalGraphReadSnapshot();
    const std::string large(40000, 'x');
    const graph::PropertyValue list(std::vector<graph::PropertyValue>{
        graph::PropertyValue(int64_t{8}), graph::PropertyValue("nested")});
    backend.replaceLogicalNodeProperties({
        {1, {{"number", graph::PropertyValue("changed")}, {"text", graph::PropertyValue(large)}, {"list", list}}},
        {2, {{"number", graph::PropertyValue(2.5)}, {"added", graph::PropertyValue(true)}}},
        {3, {{"number", graph::PropertyValue()}}}, {4, {}}});
    auto changed = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(old->readNodeProperty(1, "number"), graph::PropertyValue(int64_t{1}));
    EXPECT_EQ(old->readNodeProperty(1, "text"), graph::PropertyValue("short"));
    EXPECT_EQ(changed->readNodeProperty(1, "number"), graph::PropertyValue("changed"));
    EXPECT_EQ(changed->readNodeProperty(1, "text"), graph::PropertyValue(large));
    EXPECT_EQ(changed->readNodeProperty(1, "list"), list);
    EXPECT_EQ(changed->readNodeProperty(2, "number"), graph::PropertyValue(2.5));
    EXPECT_EQ(changed->readNodeProperty(2, "added"), graph::PropertyValue(true));
    EXPECT_EQ(changed->readNodeProperty(3, "number"), graph::PropertyValue());
    EXPECT_EQ(changed->readNodeProperty(4, "number"), graph::PropertyValue());
    old.reset(); changed.reset(); EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    auto reopened = std::make_shared<RelocatableStorageBackend>(options); reopened->open();
    auto snapshot = reopened->openLogicalGraphReadSnapshot();
    EXPECT_EQ(snapshot->readNodeProperty(1, "text"), graph::PropertyValue(large));
    EXPECT_EQ(snapshot->readNodeProperty(1, "list"), list);
    snapshot.reset(); graph::query::QueryEngine engine(reopened);
    const auto result = engine.execute("MATCH (n:Nodes) RETURN count(*) AS rows, count(n.number) AS values");
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), graph::PropertyValue(int64_t{64}));
    EXPECT_EQ(result.getRows().front().at("values").asPrimitive(), graph::PropertyValue(int64_t{62}));
}

TEST_F(ScanReadProofTest, PublicMixedBatchRetainsOldPinsAcrossEntityCowPoolCapacity) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 2U, false, 4096U)); builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto old = backend.openLogicalGraphReadSnapshot();
    LogicalGraphMutationBatch batch;
    batch.node_deletes.push_back({32, false, 0});
    batch.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 7}, {"Changed"}});
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 8}, {{"number", graph::PropertyValue(int64_t{1008})},
        {"text", graph::PropertyValue("short")}}});
    EXPECT_NO_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatch(batch)));
    auto changed = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(old->readNode(32).node_id, 32U);
    EXPECT_EQ(old->readNodeProperty(8, "number"), graph::PropertyValue(int64_t{8}));
    EXPECT_EQ(changed->readNodeProperty(8, "number"), graph::PropertyValue(int64_t{1008}));
    EXPECT_EQ(changed->readNode(7).labels, (std::vector<std::string>{"Changed"}));
    size_t before = 0, after = 0;
    old->visitNodes(SIZE_MAX, [&](const auto &) { ++before; return true; });
    changed->visitNodes(SIZE_MAX, [&](const auto &) { ++after; return true; });
    EXPECT_EQ(before, 4096U); EXPECT_EQ(after, 4095U);
    old.reset(); changed.reset(); EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend reopened(options); EXPECT_NO_THROW(reopened.open());
    EXPECT_EQ(reopened.openLogicalGraphReadSnapshot()->readNodeProperty(8, "number"), graph::PropertyValue(int64_t{1008}));
}

TEST_F(ScanReadProofTest, PublicScalarIndexAndConstraintClosureSurviveNativeUpdatesDeletionAndReopen) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true)); builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    static_cast<void>(backend.createLogicalNodeIndex({"number_index", "Nodes", {"number"}}));
    static_cast<void>(backend.createLogicalConstraint({"number_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
        LogicalGraphConstraintKind::LGCK_UNIQUE, "Nodes", {"number"}}));
    EXPECT_THROW(backend.replaceLogicalNodeProperties({{2, {{"number", graph::PropertyValue(int64_t{1})}}}}),
        std::runtime_error);
    backend.replaceLogicalNodeProperties({{2, {{"number", graph::PropertyValue(int64_t{202})}}}});
    backend.deleteLogicalNodes({{1, false, 0}});
    const auto added = backend.createLogicalNodes({{{"Nodes"}, {{"number", graph::PropertyValue(int64_t{1})}}}});
    ASSERT_EQ(added.size(), 1U);
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    auto reopened = std::make_shared<RelocatableStorageBackend>(options); ASSERT_NO_THROW(reopened->open());
    ASSERT_EQ(reopened->listLogicalIndexes(8).size(), 1U);
    graph::query::QueryEngine engine(reopened);
    auto indexed = engine.execute("MATCH (n:Nodes) WHERE n.number = 202 RETURN n.number AS number");
    ASSERT_EQ(indexed.rowCount(), 1U);
    EXPECT_EQ(indexed.getRows().front().at("number").asPrimitive(), graph::PropertyValue(int64_t{202}));
    auto aggregate = engine.execute("MATCH (n:Nodes) RETURN count(*) AS rows, sum(n.number) AS total");
    ASSERT_EQ(aggregate.rowCount(), 1U);
    EXPECT_EQ(aggregate.getRows().front().at("rows").asPrimitive(), graph::PropertyValue(int64_t{64}));
    EXPECT_EQ(aggregate.getRows().front().at("total").asPrimitive(), graph::PropertyValue(int64_t{2280}));
}

TEST_F(ScanReadProofTest, PublicThousandRowEdgesKeepPinnedPropertiesAndDetachWithNativeNodeFields) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true)); builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    std::vector<LogicalGraphEdgeCreateRequest> requests;
    for (int64_t i = 0; i < 1000; ++i) requests.push_back({1, 2, "LINK", {{"weight", graph::PropertyValue(i)}}});
    const auto edges = backend.createLogicalEdges(requests); ASSERT_EQ(edges.size(), 1000U);
    auto old = backend.openLogicalGraphReadSnapshot();
    ASSERT_NO_THROW(backend.replaceLogicalEdgeProperties({{edges.front(), {{"weight", graph::PropertyValue(int64_t{2000})}}}}));
    ASSERT_NO_THROW(backend.deleteLogicalEdges({{edges.back()}}));
    EXPECT_EQ(old->readEdgeProperty(edges.front(), "weight"), graph::PropertyValue(int64_t{0}));
    EXPECT_EQ(backend.openLogicalGraphReadSnapshot()->readEdgeProperty(edges.front(), "weight"), graph::PropertyValue(int64_t{2000}));
    ASSERT_NO_THROW(backend.deleteLogicalNodes({{1, true, 1000U}}));
    size_t oldEdges = 0, newEdges = 0, nodes = 0;
    old->visitEdges(SIZE_MAX, [&](const auto &) { ++oldEdges; return true; });
    auto current = backend.openLogicalGraphReadSnapshot();
    current->visitEdges(SIZE_MAX, [&](const auto &) { ++newEdges; return true; });
    current->visitNodes(SIZE_MAX, [&](const auto &) { ++nodes; return true; });
    EXPECT_EQ(oldEdges, 1000U); EXPECT_EQ(newEdges, 0U); EXPECT_EQ(nodes, 63U);
    old.reset(); current.reset(); EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend reopened(options); EXPECT_NO_THROW(reopened.open());
}

TEST_F(ScanReadProofTest, SingleRawCachePageUsesOwnedFallbackForWideFieldVerificationAndQueries) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 64U)); builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE; options.buffer_pool_bytes = 4096U;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); ASSERT_NO_THROW(backend->open());
    EXPECT_EQ(backend->openLogicalGraphReadSnapshot()->readNodeProperty(1, "K64"), graph::PropertyValue(int64_t{64}));
    graph::query::QueryEngine engine(backend);
    const auto result = engine.execute("MATCH (n) RETURN count(*) AS rows, sum(n.number) AS total");
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), graph::PropertyValue(int64_t{64}));
    EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), graph::PropertyValue(int64_t{2080}));
}

TEST_F(ScanReadProofTest, NodeProofDoesNotGrantRowEdgeAuthorityAndFullOpenVerificationRemainsExplicit) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true)); builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    uint64_t edgeId = 0;
    {
        RelocatableStorageBackend writer(options); writer.open();
        const auto edges = writer.createLogicalEdges({{1, 2, "LINK", {}}});
        ASSERT_EQ(edges.size(), 1U); edgeId = edges.front(); writer.close();
    }
    {
        auto stream = std::make_shared<std::fstream>(target, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::openReadOnly(io); auto pin = metadata->openSnapshot();
        const auto edge = metadata->findEdgePageLocation(pin, edgeId);
        const auto location = metadata->findPageLocation(pin, edge.page_id);
        const auto pool = metadata->findCatalog(pin, location.segment_id);
        const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(pin.superblock().data_region_offset, pool.physical_slot)
            + uint64_t(location.page_index) * RSS_PAGE_SIZE;
        std::array<std::byte, RSS_PAGE_SIZE> bytes{};
        ASSERT_EQ(io->readAt(offset, bytes.data(), bytes.size()), bytes.size());
        const auto owner = PageOwnerHeaderCodec::decode(std::span(bytes).first(RSS_PAGE_OWNER_HEADER_BYTES));
        auto page = EntityPage::open(owner, std::span(bytes).subspan(RSS_PAGE_OWNER_HEADER_BYTES));
        auto record = *page.findEdge(edge.slot_id, edge.slot_generation, edgeId);
        record.source_node_id = 999999U;
        page.replace(edge.slot_id, edge.slot_generation, edgeId, record);
        const auto payload = page.serialize();
        std::copy(payload.begin(), payload.end(), bytes.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
        io->writeAt(offset, bytes.data(), bytes.size()); io->sync();
    }
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    {
        RelocatableStorageBackend reader(options); ASSERT_NO_THROW(reader.open());
        EXPECT_EQ(reader.openLogicalGraphReadSnapshot()->readNodeProperty(1, "number"), graph::PropertyValue(int64_t{1}));
        EXPECT_THROW(static_cast<void>(reader.verifyIntegrity()), FormatException); reader.close();
    }
    std::ifstream beforeFile(target, std::ios::binary);
    const std::vector<char> before((std::istreambuf_iterator<char>(beforeFile)), {}); beforeFile.close();
    options.verify_integrity_on_open = true;
    for (const auto mode : {OpenMode::OPEN_READ_ONLY_EXISTING_FILE, OpenMode::OPEN_EXISTING_FILE}) {
        options.open_mode = mode; RelocatableStorageBackend reader(options);
        EXPECT_THROW(reader.open(), FormatException);
        std::ifstream afterFile(target, std::ios::binary);
        const std::vector<char> after((std::istreambuf_iterator<char>(afterFile)), {});
        EXPECT_EQ(after, before);
    }
}

TEST_F(ScanReadProofTest, PublicVectorClosurePreservesOldPinAndCoordinatesNativeDeletionAndRowCreation) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true)); builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    static_cast<void>(backend.createLogicalNodeVectorIndex({"list_vector", "Nodes", "list", 1U, "l2"}));
    backend.replaceLogicalNodeProperties({{2, {{"number", graph::PropertyValue(int64_t{2})},
        {"list", graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(9.0)})}}}});
    ASSERT_TRUE(backend.trainLogicalVector({"list_vector", 16U, 16U}).trained);
    LogicalGraphVectorSearchRequest request; request.index_name = "list_vector";
    request.query_vector = {7.0F}; request.top_k = 1;
    request.maximum_mapping_entries = 128; request.maximum_raw_vector_reads = 64;
    request.candidate_search_budget = 64; request.candidate_refinement_budget = 64;
    auto old = backend.openLogicalGraphVectorSearchSnapshot();
    const auto initial = old->searchLogicalVector(request);
    ASSERT_EQ(initial.hits.size(), 1U); EXPECT_EQ(initial.hits.front().node_id, 1U);
    const auto vectorValue = [](double value) {
        return graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(value)});
    };
    backend.replaceLogicalNodeProperties({{1, {{"number", graph::PropertyValue(int64_t{1})}, {"list", vectorValue(8.0)}}}});
    request.query_vector = {8.0F};
    {
        auto current = backend.openLogicalGraphVectorSearchSnapshot();
        const auto result = current->searchLogicalVector(request);
        ASSERT_EQ(result.hits.size(), 1U); EXPECT_EQ(result.hits.front().node_id, 1U);
    }
    backend.deleteLogicalNodes({{1, false, 0}});
    {
        auto current = backend.openLogicalGraphVectorSearchSnapshot();
        const auto remaining = current->searchLogicalVector(request);
        ASSERT_EQ(remaining.hits.size(), 1U); EXPECT_EQ(remaining.hits.front().node_id, 2U);
    }
    request.query_vector = {7.0F};
    const auto prior = old->searchLogicalVector(request);
    ASSERT_EQ(prior.hits.size(), 1U); EXPECT_EQ(prior.hits.front().node_id, 1U);
    const auto added = backend.createLogicalNodes({{{"Nodes"},
        {{"number", graph::PropertyValue(int64_t{100})}, {"list", vectorValue(8.0)}}}});
    ASSERT_EQ(added.size(), 1U); old.reset();
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend reopened(options); ASSERT_NO_THROW(reopened.open());
    {
        auto current = reopened.openLogicalGraphVectorSearchSnapshot(); request.query_vector = {8.0F};
        const auto result = current->searchLogicalVector(request);
        ASSERT_EQ(result.hits.size(), 1U); EXPECT_EQ(result.hits.front().node_id, added.front());
    }
    EXPECT_NO_THROW(reopened.close());
}

TEST_F(ScanReadProofTest, AdjacentGroupsCanReuseColdRemainderPoolsWithoutMutationAuthority) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 2U, false, 128U)); builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    {
        auto snapshot = backend->openLogicalGraphReadSnapshot();
        EXPECT_EQ(snapshot->readNodeProperty(1, "list"),
            graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})}));
        EXPECT_EQ(snapshot->readNodeProperty(65, "number"), graph::PropertyValue(int64_t{65}));
        EXPECT_EQ(snapshot->readNodeProperty(128, "number"), graph::PropertyValue(int64_t{128}));
    }
    graph::query::QueryEngine engine(backend);
    auto result = engine.execute("MATCH (n:Nodes) RETURN count(*) AS rows, sum(n.number) AS total");
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), graph::PropertyValue(int64_t{128}));
    EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), graph::PropertyValue(int64_t{8256}));
}

TEST_F(ScanReadProofTest, TerminalPropertylessNodeRemainsVisibleAndSuppliesAbsentKnownFields) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 2U, true)); builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    {
        auto snapshot = backend->openLogicalGraphReadSnapshot();
        EXPECT_EQ(snapshot->readNodeProperty(UINT64_MAX, "number"), graph::PropertyValue());
    }
    graph::query::QueryEngine engine(backend);
    auto result = engine.execute("MATCH (n) RETURN count(*) AS rows, count(n.number) AS values, sum(n.number) AS total");
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), graph::PropertyValue(int64_t{65}));
    EXPECT_EQ(result.getRows().front().at("values").asPrimitive(), graph::PropertyValue(int64_t{64}));
    EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), graph::PropertyValue(int64_t{2080}));
}

TEST_F(ScanReadProofTest, PublicNamedFieldConsumerReadsScalarTextAndColdValuesUnderMembershipProof) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    RelocatableStorageBackendOptions options; options.data_path = target;
    RelocatableStorageBackend backend(options); ASSERT_NO_THROW(backend.open());
    auto readMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    graph::utils::OwnedAllocationScope readOwner(readMemory);
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    auto *provider = dynamic_cast<LogicalGraphNodeScanProvider *>(snapshot.get()); ASSERT_NE(provider, nullptr);
    const std::array<std::string_view, 3> keys{"number", "text", "list"};
    size_t scalarCount = 0, textCount = 0, coldCount = 0, missingCount = 0, saved = 0;
    graph::debug::PerfTrace::ScopedObserver observer(&saved,
        [](void *p, std::string_view key, int64_t value, bool duration) noexcept {
            if (!duration && key == "native_graph.hybrid.certified_owner_route_checks_saved")
                *static_cast<size_t *>(p) += size_t(value);
        });
    const PropertyColumnBatchReadSinks sinks{
        [&](auto rows, size_t column, auto, auto bits) {
            EXPECT_EQ(column, 0U);
            for (size_t i = 0; i < rows.size(); ++i) { EXPECT_EQ(bits[i], rows[i] + 1U); ++scalarCount; }
        },
        [&](auto rows, size_t column, auto texts) {
            EXPECT_EQ(column, 1U); EXPECT_EQ(rows.size(), texts.size());
            for (auto text : texts) { EXPECT_EQ(text, "short"); ++textCount; }
        }};
    const LogicalGraphReadSnapshot::PropertyCellSink values = [&](size_t row, size_t column, auto value) {
        EXPECT_EQ(column, 2U);
        if (!value) { EXPECT_GT(row, 0U); ++missingCount; return; }
        EXPECT_EQ(row, 0U);
        EXPECT_EQ(*value, graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})}));
        ++coldCount;
    };
    const auto result = provider->tryVisitNodeScanBatches(SIZE_MAX, 512, {"Nodes", "Nodes"}, keys,
        [&](auto ids, size_t examined, const auto &source) {
            EXPECT_EQ(ids.size(), 64U); EXPECT_EQ(examined, 64U);
            source(ids, values, sinks, [](size_t) {});
            const std::array<uint64_t, 1> foreign{65};
            EXPECT_THROW(source(foreign, values, sinks, [](size_t) {}), std::invalid_argument);
            return true;
        }, [](size_t) {});
    ASSERT_TRUE(result.has_value()); EXPECT_TRUE(*result);
    EXPECT_EQ(scalarCount, 64U); EXPECT_EQ(textCount, 64U); EXPECT_EQ(coldCount, 1U); EXPECT_EQ(missingCount, 63U); EXPECT_GE(saved, 64U);
    size_t calls = 0;
    EXPECT_EQ(provider->tryVisitNodeScanBatches(SIZE_MAX, 512, {"absent"}, keys,
        [&](auto, size_t, const auto &) { ++calls; return true; }, [](size_t) {}), true);
    EXPECT_EQ(calls, 0U);
}

TEST_F(ScanReadProofTest, QueryConsumerReturnsOwnedFilterAndGroupedResultsFromPersistentMembership) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    RelocatableStorageBackendOptions options; options.data_path = target;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    size_t batches = 0;
    graph::debug::PerfTrace::ScopedObserver observer(&batches,
        [](void *p, std::string_view key, int64_t value, bool duration) noexcept {
            if (!duration && key == "native_graph.read.stored_membership_scan_batches")
                *static_cast<size_t *>(p) += size_t(value);
        });
    graph::query::QueryEngine engine(backend);
    auto result = engine.execute("MATCH (n:Nodes) WHERE n.number >= 1 RETURN n.text AS text, count(*) AS rows, sum(n.number) AS total");
    ASSERT_EQ(result.rowCount(), 1U);
    const auto &row = result.getRows().front();
    EXPECT_EQ(row.at("text").asPrimitive(), graph::PropertyValue("short"));
    EXPECT_EQ(row.at("rows").asPrimitive(), graph::PropertyValue(int64_t{64}));
    EXPECT_EQ(row.at("total").asPrimitive(), graph::PropertyValue(int64_t{2080}));
    EXPECT_GT(batches, 0U); batches = 0;
    auto filtered = engine.execute("MATCH (n) WHERE n.number >= 33 RETURN n.text AS text, count(*) AS rows, sum(n.number) AS total");
    ASSERT_EQ(filtered.rowCount(), 1U);
    EXPECT_EQ(filtered.getRows().front().at("rows").asPrimitive(), graph::PropertyValue(int64_t{32}));
    EXPECT_EQ(filtered.getRows().front().at("total").asPrimitive(), graph::PropertyValue(int64_t{1552}));
    EXPECT_GT(batches, 0U);
}

TEST_F(ScanReadProofTest, OversizedLabelGroupsUseCompletePrimaryLabelsWithoutTruncationOrAnUnlabelledScanPenalty) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, true));
    RelocatableStorageBackendOptions options; options.data_path = target;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); ASSERT_NO_THROW(backend->open());
    size_t fallbackGroups = 0;
    graph::debug::PerfTrace::ScopedObserver observer(&fallbackGroups,
        [](void *p, std::string_view key, int64_t value, bool duration) noexcept {
            if (!duration && key == "native_graph.read.primary_label_fallback_groups")
                *static_cast<size_t *>(p) += size_t(value);
        });
    auto readMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    graph::utils::OwnedAllocationScope readOwner(readMemory);
    {
        auto snapshot = backend->openLogicalGraphReadSnapshot();
        auto *provider = dynamic_cast<LogicalGraphNodeScanProvider *>(snapshot.get()); ASSERT_NE(provider, nullptr);
        const std::array<std::string_view, 1> keys{"number"};
        size_t selected = 0, examined = 0;
        auto result = provider->tryVisitNodeScanBatches(SIZE_MAX, 512, {"L1", "L65", "L1"}, keys,
            [&](auto ids, size_t count, const auto &) {
                selected += ids.size(); examined += count;
                EXPECT_EQ(ids.size(), 1U); if (!ids.empty()) EXPECT_EQ(ids[0], 1U); return true;
            }, [](size_t) {});
        ASSERT_TRUE(result.has_value()); EXPECT_TRUE(*result);
        EXPECT_EQ(selected, 1U); EXPECT_EQ(examined, 64U); EXPECT_EQ(fallbackGroups, 1U);
        selected = examined = 0; fallbackGroups = 0;
        result = provider->tryVisitNodeScanBatches(SIZE_MAX, 512, {}, keys,
            [&](auto ids, size_t count, const auto &) { selected += ids.size(); examined += count; return true; },
            [](size_t) {});
        ASSERT_TRUE(result.has_value()); EXPECT_TRUE(*result);
        EXPECT_EQ(selected, 64U); EXPECT_EQ(examined, 64U); EXPECT_EQ(fallbackGroups, 0U);
        selected = examined = 0;
        result = provider->tryVisitNodeScanBatches(SIZE_MAX, 512, {"L1", "L2"}, keys,
            [&](auto ids, size_t count, const auto &) { selected += ids.size(); examined += count; return true; },
            [](size_t) {});
        ASSERT_TRUE(result.has_value()); EXPECT_TRUE(*result);
        EXPECT_EQ(selected, 0U); EXPECT_EQ(examined, 64U);
    }
    fallbackGroups = 0;
    graph::query::QueryEngine engine(backend);
    auto result = engine.execute("MATCH (n:L1) WHERE n.number >= 1 RETURN n.text AS text, count(*) AS rows, sum(n.number) AS total");
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), graph::PropertyValue(int64_t{1}));
    EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), graph::PropertyValue(int64_t{1}));
    EXPECT_GT(fallbackGroups, 0U);
}

TEST_F(ScanReadProofTest, CompletedImageFieldUpdatePreservesMembershipAndOldPinThenReopensWithFreshProof) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    RelocatableStorageBackendOptions options; options.data_path = target;
    {
        RelocatableStorageBackend before(options); before.open();
        { auto snapshot = before.openLogicalGraphReadSnapshot();
          EXPECT_EQ(snapshot->readNodeProperty(1, "number"), graph::PropertyValue(int64_t{1})); }
        before.close();
    }
    {
        auto readMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        graph::utils::OwnedAllocationScope readOwner(readMemory);
        auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
        auto old = store().openSnapshot(); const auto oldDesc = targetDescriptor(old);
        SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore large(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore properties(pages, large, store());
        const auto base = old.superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{
            {{EntityPropertyOwnerKind::EPOK_NODE, 1}, 1,
                graph::OwnedPropertyValue(graph::PropertyValue(int64_t{777}))}}};
        auto writeMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        NativeHybridPropertyColumnWrite change{base.database_id, base.generation, base.hybrid_property_descriptor_root,
            cells, writeMemory, PhysicalSlotId(1), PhysicalSlotId(127), 989, nullptr, true};
        const auto published = store().mutateHybridPropertyColumns(change, LogSequenceNumber(base.checkpoint_lsn.value() + 1U));
        ASSERT_GT(published.generation, base.generation); EXPECT_EQ(writeMemory->used(), 0U);
        auto current = store().openSnapshot(); const auto desc = targetDescriptor(current);
        constexpr auto ordinal = static_cast<size_t>(HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
        EXPECT_EQ(desc.roots[ordinal], oldDesc.roots[ordinal]);
        EXPECT_EQ(desc.scan_live_node_count, oldDesc.scan_live_node_count);
        EXPECT_EQ(desc.scan_membership_group_count, oldDesc.scan_membership_group_count);
        const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
        const auto oldValue = properties.readEntityPropertyOptional(old, owner, 1);
        const auto newValue = properties.readEntityPropertyOptional(current, owner, 1);
        ASSERT_TRUE(oldValue); ASSERT_TRUE(newValue);
        EXPECT_EQ(oldValue->value, graph::PropertyValue(int64_t{1}));
        EXPECT_EQ(newValue->value, graph::PropertyValue(int64_t{777}));
        size_t calls = 0;
        EXPECT_THROW(static_cast<void>(CertifiedScanMembership::visit(verified, targetIo(), store(), old, source,
            SIZE_MAX, 512, {}, [&](auto, size_t, const auto &) { ++calls; return true; })), FormatException);
        EXPECT_EQ(calls, 0U);
        auto fresh = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
        EXPECT_TRUE(CertifiedScanMembership::visit(fresh, targetIo(), store(), current, source, SIZE_MAX, 512, {},
            [&](auto ids, size_t examined, const auto &) { ++calls; EXPECT_EQ(ids.size(), 64U); EXPECT_EQ(examined, 64U); return true; }));
        EXPECT_EQ(calls, 1U);
    }
    builder.reset();
    auto reopened = std::make_shared<RelocatableStorageBackend>(options); ASSERT_NO_THROW(reopened->open());
    graph::query::QueryEngine engine(reopened);
    const auto result = engine.execute("MATCH (n:Nodes) WHERE n.number = 777 RETURN n.text AS text, count(*) AS rows, sum(n.number) AS total");
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), graph::PropertyValue(int64_t{1}));
    EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), graph::PropertyValue(int64_t{777}));
}

TEST_F(ScanReadProofTest, LogicalLabelMutationPublishesCompleteMembershipWithEntityAndCatalogRoots) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope owner(memory);
    auto old = store().openSnapshot(); const auto oldDesc = targetDescriptor(old);
    LogicalGraphMutationBatch batch;
    batch.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {"Changed"}});
    const auto result = mutateImage(batch); ASSERT_TRUE(result.committed());
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    auto current = store().openSnapshot(); const auto desc = targetDescriptor(current);
    EXPECT_EQ(desc.scan_live_node_count, oldDesc.scan_live_node_count);
    EXPECT_EQ(desc.scan_membership_group_count, oldDesc.scan_membership_group_count);
    auto fresh = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore trees(pages, store()); SystemCatalog catalog(trees, store());
    const auto changed = catalog.findSchemaToken(current, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Changed");
    ASSERT_TRUE(changed);
    size_t selected = 0;
    EXPECT_TRUE(CertifiedScanMembership::visit(fresh, targetIo(), store(), current, source, SIZE_MAX, 512,
        std::span(&*changed, 1), [&](auto ids, size_t examined, const auto &) {
            selected += ids.size(); EXPECT_EQ(examined, 64U); if (!ids.empty()) EXPECT_EQ(ids[0], 1U); return true;
        }));
    EXPECT_EQ(selected, 1U);
    const auto original = catalog.findSchemaToken(old, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Nodes"); ASSERT_TRUE(original);
    selected = 0;
    static_cast<void>(scan_candidate_detail::streamStoredMembership(targetIo(), store(), old, source, SIZE_MAX,
        [&](const auto &membership) { selected += std::popcount(membership.select(std::span(&*original, 1))); return true; }));
    EXPECT_EQ(selected, 64U);
}

TEST_F(ScanReadProofTest, LogicalCreateAndDeleteMaintainNewMembershipGroupsAndPinnedRowFallback) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope owner(memory);
    auto old = store().openSnapshot();
    LogicalGraphMutationBatch create;
    create.node_creates.push_back({{"Nodes"}, {{"number", graph::PropertyValue(int64_t{909})}, {"text", graph::PropertyValue("new")}}});
    const auto added = mutateImage(create); ASSERT_TRUE(added.committed()); ASSERT_EQ(added.result.created_node_ids.size(), 1U);
    const auto id = added.result.created_node_ids.front(); ASSERT_GT(id, 64U);
    auto created = store().openSnapshot(); const auto createdDesc = targetDescriptor(created);
    EXPECT_EQ(createdDesc.scan_live_node_count, 65U); EXPECT_EQ(createdDesc.scan_membership_group_count, 2U);
    ASSERT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore values(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages, values, store());
    const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, id};
    EXPECT_FALSE(properties.readEntityPropertyOptional(old, key, 1));
    const auto value = properties.readEntityPropertyOptional(created, key, 1); ASSERT_TRUE(value);
    EXPECT_EQ(value->value, graph::PropertyValue(int64_t{909}));
    LogicalGraphMutationBatch erase; erase.node_deletes.push_back({id, false, 0});
    const auto removed = mutateImage(erase); ASSERT_TRUE(removed.committed());
    auto current = store().openSnapshot(); const auto desc = targetDescriptor(current);
    EXPECT_EQ(desc.scan_live_node_count, 64U); EXPECT_EQ(desc.scan_membership_group_count, 1U);
    ASSERT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    EXPECT_FALSE(properties.readEntityPropertyOptional(current, key, 1));
    const auto retained = properties.readEntityPropertyOptional(created, key, 1); ASSERT_TRUE(retained);
    EXPECT_EQ(retained->value, graph::PropertyValue(int64_t{909}));
    size_t oldCount = 0, createdCount = 0;
    static_cast<void>(scan_candidate_detail::streamStoredMembership(targetIo(), store(), old, source, SIZE_MAX,
        [&](const auto &membership) { oldCount += std::popcount(membership.live); return true; }));
    static_cast<void>(scan_candidate_detail::streamStoredMembership(targetIo(), store(), created, source, SIZE_MAX,
        [&](const auto &membership) { createdCount += std::popcount(membership.live); return true; }));
    EXPECT_EQ(oldCount, 64U); EXPECT_EQ(createdCount, 65U);
}

TEST_F(ScanReadProofTest, LogicalDeletionCoordinatesNativeFieldsColdValuesAndMembershipInOneRoot) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope owner(memory);
    auto old = store().openSnapshot();
    LogicalGraphMutationBatch erase; erase.node_deletes.push_back({1, false, 0});
    const auto result = mutateImage(erase); ASSERT_TRUE(result.committed());
    auto current = store().openSnapshot(); const auto desc = targetDescriptor(current);
    EXPECT_EQ(desc.scan_live_node_count, 63U); EXPECT_EQ(desc.scan_membership_group_count, 1U);
    ASSERT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore values(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages, values, store());
    const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 1};
    EXPECT_FALSE(properties.readEntityOptional(current, key));
    const auto retained = properties.readEntityOptional(old, key); ASSERT_TRUE(retained); EXPECT_EQ(retained->entries.size(), 3U);
    auto proof = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    size_t selected = 0;
    EXPECT_TRUE(CertifiedScanMembership::visit(proof, targetIo(), store(), current, source, SIZE_MAX, 512, {},
        [&](auto ids, size_t examined, const auto &) {
            selected += ids.size(); EXPECT_EQ(examined, 63U);
            EXPECT_EQ(std::find(ids.begin(), ids.end(), 1U), ids.end()); return true;
        }));
    EXPECT_EQ(selected, 63U);
}

TEST_F(ScanReadProofTest, DetachDeletionCoordinatesANativeNodeWithARowFallbackEdge) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope owner(memory);
    LogicalGraphMutationBatch create;
    create.edge_creates.push_back({
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2},
        "Links", {{"weight", graph::PropertyValue(int64_t{7})}}});
    const auto added = mutateImage(create); ASSERT_TRUE(added.committed());
    ASSERT_EQ(added.result.created_edge_ids.size(), 1U);
    auto old = store().openSnapshot();
    EXPECT_EQ(targetDescriptor(old).scan_live_node_count, 64U);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    LogicalGraphMutationBatch erase; erase.node_deletes.push_back({1, true, 64});
    ASSERT_TRUE(mutateImage(erase).committed());
    auto current = store().openSnapshot();
    EXPECT_EQ(targetDescriptor(current).scan_live_node_count, 63U);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    EntityPageStore entities(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    const std::array ids{added.result.created_edge_ids.front()};
    const auto prior = entities.readEdgesSorted(old, ids, [](size_t) {});
    const auto removed = entities.readEdgesSorted(current, ids, [](size_t) {});
    ASSERT_EQ(prior.size(), 1U); EXPECT_TRUE(prior.front());
    ASSERT_EQ(removed.size(), 1U); EXPECT_FALSE(removed.front());
    auto proof = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    size_t selected = 0;
    EXPECT_TRUE(CertifiedScanMembership::visit(proof, targetIo(), store(), current, source, SIZE_MAX, 512, {},
        [&](auto batch, size_t, const auto &) { selected += batch.size(); return true; }));
    EXPECT_EQ(selected, 63U);
}

TEST_F(ScanReadProofTest, NodeOnlyMembershipWorkspaceMustBeAdmittedByTheCallerBeforePublication) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    const auto before = store().openSnapshot().superblock();
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(512U * 1024U);
    {
        graph::utils::OwnedAllocationScope owner(tiny);
        LogicalGraphMutationBatch batch;
        batch.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
            LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {"BudgetDenied"}});
        targetIo()->resetStats();
        EXPECT_THROW(static_cast<void>(mutateImage(batch)), graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(targetIo()->stats().write_calls, 0U);
    }
    EXPECT_EQ(store().openSnapshot().superblock(), before); EXPECT_EQ(tiny->used(), 0U);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
}

TEST_F(ScanReadProofTest, NativeFieldAndDeletionWorkspacesCannotEscapeTheCallerBudget) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    const auto before = store().openSnapshot().superblock();
    for (const bool deletion : {false, true}) {
        auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(512U * 1024U);
        {
            graph::utils::OwnedAllocationScope owner(tiny);
            LogicalGraphMutationBatch batch;
            if (deletion) batch.node_deletes.push_back({1, false, 0});
            else batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
                LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
                {{"number", graph::PropertyValue(int64_t{999})}}});
            targetIo()->resetStats();
            EXPECT_THROW(static_cast<void>(mutateImage(batch)), graph::query::execution::MaterializedByteLimitExceeded);
            EXPECT_EQ(targetIo()->stats().write_calls, 0U);
        }
        EXPECT_EQ(tiny->used(), 0U);
        EXPECT_EQ(store().openSnapshot().superblock(), before);
    }
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
}

TEST_F(ScanReadProofTest, PartialDeletionBootstrapWriteRecoversTheWholeOldGraphAndMembership) {
    checkDeletionPublicationFailure(BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, false);
}
TEST_F(ScanReadProofTest, CompleteDeletionBootstrapWriteRecoversTheWholeNewGraphAndMembership) {
    checkDeletionPublicationFailure(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE, true);
}

TEST_F(ScanReadProofTest, LogicalRelabelTransitionsIntoAndOutOfPrimaryLabelFallback) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope owner(memory);
    LogicalGraphMutationBatch wide;
    std::vector<std::string> names;
    for (size_t i = 0; i < 65U; ++i) names.push_back("Wide" + std::to_string(i));
    wide.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, names});
    ASSERT_TRUE(mutateImage(wide).committed());
    ASSERT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    auto current = store().openSnapshot();
    size_t fallbacks = 0;
    static_cast<void>(scan_candidate_detail::streamStoredMembership(targetIo(), store(), current, source, SIZE_MAX,
        [&](const auto &membership) { fallbacks += membership.labels_from_primary; EXPECT_EQ(membership.label_count, 0U); return true; }));
    EXPECT_EQ(fallbacks, 1U);
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore trees(pages, store()); SystemCatalog catalog(trees, store());
    const auto token = catalog.findSchemaToken(current, SystemCatalogSchemaTokenKind::SCSTK_LABEL, names.back()); ASSERT_TRUE(token);
    auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    size_t selected = 0;
    EXPECT_TRUE(CertifiedScanMembership::visit(verified, targetIo(), store(), current, source, SIZE_MAX, 512,
        std::span(&*token, 1), [&](auto ids, size_t examined, const auto &) {
            selected += ids.size(); EXPECT_EQ(examined, 64U); if (!ids.empty()) EXPECT_EQ(ids[0], 1U); return true;
        }));
    EXPECT_EQ(selected, 1U);
    LogicalGraphMutationBatch narrow;
    narrow.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {"Nodes"}});
    ASSERT_TRUE(mutateImage(narrow).committed());
    ASSERT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    auto final = store().openSnapshot(); fallbacks = 0;
    static_cast<void>(scan_candidate_detail::streamStoredMembership(targetIo(), store(), final, source, SIZE_MAX,
        [&](const auto &membership) { fallbacks += membership.labels_from_primary; EXPECT_EQ(membership.label_count, 1U); return true; }));
    EXPECT_EQ(fallbacks, 0U);
}

TEST_F(ScanReadProofTest, MixedLogicalNodeChangesPublishFieldsLabelsAndBothMembershipGroupsTogether) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope owner(memory);
    auto old = store().openSnapshot();
    LogicalGraphMutationBatch batch;
    batch.node_creates.push_back({{"Nodes"}, {{"number", graph::PropertyValue(int64_t{909})}}});
    batch.node_deletes.push_back({1, false, 0});
    batch.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2}, {"Changed"}});
    const auto result = mutateImage(batch); ASSERT_TRUE(result.committed()); ASSERT_EQ(result.result.created_node_ids.size(), 1U);
    const auto created = result.result.created_node_ids.front();
    auto current = store().openSnapshot(); const auto desc = targetDescriptor(current);
    EXPECT_EQ(desc.scan_live_node_count, 64U); EXPECT_EQ(desc.scan_membership_group_count, 2U);
    ASSERT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    size_t count = 0; bool foundCreated = false, foundDeleted = false;
    static_cast<void>(scan_candidate_detail::streamStoredMembership(targetIo(), store(), current, source, SIZE_MAX,
        [&](const auto &membership) {
            count += std::popcount(membership.live);
            if (membership.group_id == 1U + (created - 1U) / 64U)
                foundCreated = bool(membership.live & (UINT64_C(1) << ((created - 1U) % 64U)));
            if (membership.group_id == 1U) foundDeleted = bool(membership.live & 1U);
            return true;
        }));
    EXPECT_EQ(count, 64U); EXPECT_TRUE(foundCreated); EXPECT_FALSE(foundDeleted);
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore values(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages, values, store());
    const auto retained = properties.readEntityOptional(old, {EntityPropertyOwnerKind::EPOK_NODE, 1});
    ASSERT_TRUE(retained); EXPECT_EQ(retained->entries.size(), 3U);
    const auto value = properties.readEntityPropertyOptional(current, {EntityPropertyOwnerKind::EPOK_NODE, created}, 1);
    ASSERT_TRUE(value); EXPECT_EQ(value->value, graph::PropertyValue(int64_t{909}));
}

TEST_F(ScanReadProofTest, CompletePropertyReplacementAddsThe65thFieldThroughGeneralFallback) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 64U));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope owner(memory);
    LogicalGraphPropertyMutationRequest replacement;
    replacement.entity = {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1};
    for (size_t token = 1; token <= 64U; ++token) {
        const std::string name = token == 1 ? "number" : token == 2 ? "text" : token == 3 ? "list" : "K" + std::to_string(token);
        replacement.properties.push_back({name, graph::PropertyValue(int64_t(token))});
    }
    replacement.properties.push_back({"extra", graph::PropertyValue(int64_t{777})});
    LogicalGraphMutationBatch batch; batch.property_replacements.push_back(std::move(replacement));
    ASSERT_TRUE(mutateImage(batch).committed());
    ASSERT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
    auto pin = store().openSnapshot(); const auto desc = targetDescriptor(pin);
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore values(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages, values, store());
    BTreePageStore trees(pages, store()); SystemCatalog catalog(trees, store());
    const auto extra = catalog.findPropertyToken(pin, "extra"); ASSERT_TRUE(extra);
    const auto value = properties.readEntityPropertyOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *extra);
    ASSERT_TRUE(value); EXPECT_EQ(value->value, graph::PropertyValue(int64_t{777}));
    const auto full = properties.readEntityOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1});
    ASSERT_TRUE(full); EXPECT_EQ(full->entries.size(), 65U);
    const auto binding = HybridPropertyDirectoryCodec::bindRoot(desc, pin, HybridPropertyRootOrdinal::HPRO_NODE_GROUP);
    HybridPropertyGroupDirectoryMap groups(binding.root.index_id, trees, store(), binding);
    const auto locator = groups.find(pin, HybridPropertyDirectoryCodec::groupKey(1)); ASSERT_TRUE(locator);
    const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*locator);
    HybridPropertyFieldPageStore fields(targetIo(), store(), pin);
    const auto page = fields.read(pin, slot.page_id);
    const auto group = page.view().readGroup(slot, EntityPropertyOwnerKind::EPOK_NODE, 1, desc.semantic_generation);
    EXPECT_LE(group.columns, 64U);
    EXPECT_EQ(desc.scan_live_node_count, 64U);
}

TEST_F(ScanReadProofTest, PublicBackendRejectsBuildingBeforeReadAuthorityAndCreatesNoScanFile) {
    ASSERT_NO_FATAL_FAILURE(makeImage(false));
    RelocatableStorageBackendOptions options;
    options.data_path = target;
    RelocatableStorageBackend backend(options);
    EXPECT_THROW(backend.open(), FormatException);
    options.data_path = target.string() + ".new";
    options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(options)), FormatException);
    EXPECT_FALSE(std::filesystem::exists(options.data_path));
}

TEST_F(ScanReadProofTest, BuildingAndInvalidAccountsCannotMintCompleteAuthority) {
    ASSERT_NO_FATAL_FAILURE(makeImage(false));
    EXPECT_THROW(static_cast<void>(VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control)), FormatException);
    EXPECT_EQ(control->used(), 0U); EXPECT_EQ(work->used(), 0U);
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U);
    targetIo()->resetStats();
    EXPECT_THROW(static_cast<void>(VerifiedScanRoot::verifyComplete(targetIo(), store(), tiny, control)),
        graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(targetIo()->stats().read_calls, 0U); EXPECT_EQ(tiny->used(), 0U);
    auto unmanaged = std::shared_ptr<std::pmr::memory_resource>(std::pmr::new_delete_resource(), [](auto *) {});
    EXPECT_THROW(static_cast<void>(VerifiedScanRoot::verifyComplete(targetIo(), store(), work, unmanaged)), std::invalid_argument);
    EXPECT_EQ(targetIo()->stats().read_calls, 0U);
}

TEST_F(ScanReadProofTest, ScopeBindsSelectedIdsDescriptorStoreAndIoAndCopiesRequestedIds) {
    ASSERT_NO_FATAL_FAILURE(makeImage());
    auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    EXPECT_GT(control->used(), 0U); EXPECT_EQ(work->used(), 0U);
    auto pin = store().openSnapshot(); const auto desc = targetDescriptor(pin);
    const std::array<EntityPropertyKey, 1> one{{{EntityPropertyOwnerKind::EPOK_NODE, 1}}};
    const std::array<EntityPropertyKey, 1> other{{{EntityPropertyOwnerKind::EPOK_NODE, 65}}};
    const std::array<EntityPropertyKey, 1> edge{{{EntityPropertyOwnerKind::EPOK_EDGE, 1}}};
    auto foreign = RelocatableMetadataStore::openReadOnly(targetIo()); auto foreignPin = foreign->openSnapshot();
    auto foreignIo = std::make_shared<StorageIO>(nullptr, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto changed = desc; ++changed.scan_live_node_count;
    auto rows = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, one));
    size_t callbacks = 0;
    EXPECT_TRUE(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 512,
        std::array<uint64_t, 1>{7}, [&](auto ids, size_t examined, const auto &grant) {
            ++callbacks; EXPECT_EQ(examined, 2U); EXPECT_EQ(ids.size(), 1U); EXPECT_EQ(ids[0], 1U);
            EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, one));
            std::array<uint64_t, 1> requested{1};
            grant.read(requested, [&] {
                requested[0] = 65;
                EXPECT_TRUE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, one));
                EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, other));
                EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, edge));
                EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, changed, one));
                EXPECT_FALSE(ScanBatchReadScope::covers(*foreign, *targetIo(), foreignPin, desc, one));
                EXPECT_FALSE(ScanBatchReadScope::covers(store(), *foreignIo, pin, desc, one));
            });
            size_t calls = 0;
            EXPECT_THROW(grant.read(std::array<uint64_t, 1>{65}, [&] { ++calls; }), std::invalid_argument);
            EXPECT_THROW(grant.read(std::array<uint64_t, 2>{1, 1}, [&] { ++calls; }), std::invalid_argument);
            EXPECT_EQ(calls, 0U);
            return true;
        }));
    EXPECT_EQ(callbacks, 1U); EXPECT_EQ(rows->used(), 0U);
    EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, one));
}

TEST_F(ScanReadProofTest, PrefixStopsCancellationAndConsumerFailureNeverReplayOrLeaveAuthority) {
    ASSERT_NO_FATAL_FAILURE(makeImage());
    auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    auto pin = store().openSnapshot(); const auto desc = targetDescriptor(pin);
    auto rows = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const std::array<EntityPropertyKey, 1> one{{{EntityPropertyOwnerKind::EPOK_NODE, 1}}};
    size_t calls = 0;
    EXPECT_FALSE(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 1, {},
        [&](auto, size_t, const auto &) { ++calls; return false; }));
    EXPECT_EQ(calls, 1U); calls = 0;
    EXPECT_THROW(static_cast<void>(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 1, {},
        [&](auto ids, size_t, const auto &grant) {
            ++calls; grant.read(ids, [&] { throw std::runtime_error("stop property consumer"); }); return true;
        })), std::runtime_error);
    EXPECT_EQ(calls, 1U); EXPECT_EQ(rows->used(), 0U);
    EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, one));
    struct Cancel final : CancellationCheckpoint {
        bool stop = false;
        void check() const override { if (stop) throw std::runtime_error("cancel certified prefix"); }
    } cancel;
    calls = 0;
    EXPECT_THROW(static_cast<void>(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 1, {},
        [&](auto, size_t, const auto &) { ++calls; cancel.stop = true; return true; }, &cancel)), std::runtime_error);
    EXPECT_EQ(calls, 1U); EXPECT_EQ(rows->used(), 0U);
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(64U);
    targetIo()->resetStats();
    EXPECT_THROW(static_cast<void>(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, tiny, SIZE_MAX, 1, {},
        [](auto, size_t, const auto &) { return true; })), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(targetIo()->stats().read_calls, 0U); EXPECT_EQ(tiny->used(), 0U);
}

TEST_F(ScanReadProofTest, IoMutationInvalidatesCurrentAndFutureGrantsUntilFullReverification) {
    ASSERT_NO_FATAL_FAILURE(makeImage());
    auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    auto pin = store().openSnapshot(); const auto desc = targetDescriptor(pin);
    auto rows = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const std::array<EntityPropertyKey, 1> one{{{EntityPropertyOwnerKind::EPOK_NODE, 1}}};
    size_t calls = 0;
    EXPECT_THROW(static_cast<void>(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 1, {},
        [&](auto ids, size_t, const auto &grant) {
            ++calls;
            grant.read(ids, [&] {
                std::array<std::byte, 8> unchanged{};
                EXPECT_EQ(targetIo()->readAt(0, unchanged.data(), unchanged.size()), unchanged.size());
                targetIo()->writeAt(0, unchanged.data(), unchanged.size());
                EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, one));
            });
            return true;
        })), FormatException);
    EXPECT_EQ(calls, 1U); EXPECT_EQ(rows->used(), 0U);
    EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, one));
    EXPECT_THROW(static_cast<void>(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 512, {},
        [](auto, size_t, const auto &) { return true; })), FormatException);
    verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    calls = 0;
    EXPECT_TRUE(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 512, {},
        [&](auto ids, size_t, const auto &) { calls += ids.size(); return true; }));
    EXPECT_EQ(calls, 2U); EXPECT_EQ(rows->used(), 0U);
}

TEST_F(ScanReadProofTest, NestedReadsRestoreOuterAuthorityAndMovingTheCallerHandleCannotRetagTheBatch) {
    ASSERT_NO_FATAL_FAILURE(makeImage());
    auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    auto pin = store().openSnapshot(); const auto desc = targetDescriptor(pin);
    auto rows = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const std::array<EntityPropertyKey, 2> both{{{EntityPropertyOwnerKind::EPOK_NODE, 1}, {EntityPropertyOwnerKind::EPOK_NODE, 65}}};
    std::optional<VerifiedScanRoot> moved;
    const auto charged = control->used();
    EXPECT_TRUE(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 512, {},
        [&](auto ids, size_t, const auto &grant) {
            moved.emplace(std::move(verified));
            grant.read(ids, [&] {
                EXPECT_TRUE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, both));
                EXPECT_THROW(grant.read(std::array<uint64_t, 1>{65}, [&] {
                    EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, std::span(both).first(1)));
                    EXPECT_TRUE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, std::span(both).last(1)));
                    throw std::runtime_error("nested read");
                }), std::runtime_error);
                EXPECT_TRUE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, both));
            });
            return true;
        }));
    EXPECT_EQ(rows->used(), 0U); EXPECT_EQ(control->used(), charged);
    EXPECT_FALSE(ScanBatchReadScope::covers(store(), *targetIo(), pin, desc, both));
}

TEST_F(ScanReadProofTest, ValidMembershipChecksumsCannotReplaceCompletePrimaryLabelVerification) {
    ASSERT_NO_FATAL_FAILURE(makeImage());
    {
    auto pin = store().openSnapshot(); const auto desc = targetDescriptor(pin);
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore trees(pages, store());
    const auto binding = ScanMembershipDirectoryCodec::bind(desc, pin);
    ScanMembershipDirectoryMap directory(binding.root.index_id, trees, store(), binding);
    const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*directory.find(pin, ScanMembershipDirectoryCodec::key(1)));
    const auto location = store().findPageLocation(pin, slot.page_id);
    const auto pool = store().findCatalog(pin, location.segment_id);
    const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(pin.superblock().data_region_offset, pool.physical_slot) +
        uint64_t(location.page_index) * RSS_PAGE_SIZE;
    std::array<std::byte, RSS_PAGE_SIZE> frame{};
    ASSERT_EQ(targetIo()->readAt(offset, frame.data(), frame.size()), frame.size());
    auto image = HybridPropertyPageImage::open(std::span(frame).subspan(RSS_PAGE_OWNER_HEADER_BYTES),
        slot.page_id, location.generation, pin.superblock().generation, source);
    auto record = image.readScanMembership(slot, pin.superblock().database_id, 1, desc.semantic_generation);
    record.labels[0].token = 13;
    image.replaceScanMembership(slot, record, desc.semantic_generation);
    const auto bytes = image.serialize();
    std::copy(bytes.begin(), bytes.end(), frame.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
    targetIo()->writeAt(offset, frame.data(), frame.size()); targetIo()->sync();
    HybridPropertyFieldPageStore fields(targetIo(), store(), pin);
    const auto authenticated = fields.read(pin, slot.page_id);
    EXPECT_EQ(authenticated.view().readScanMembership(slot, pin.superblock().database_id, 1,
        desc.semantic_generation).select(std::array<uint64_t, 1>{13}), 1U);
    EXPECT_THROW(static_cast<void>(VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control)), FormatException);
    EXPECT_EQ(control->used(), 0U); EXPECT_EQ(work->used(), 0U);
    }
    builder.reset();
    const auto fileBytes = [&] {
        std::ifstream file(target, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    const auto before = fileBytes();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options);
    EXPECT_THROW(backend.open(), FormatException);
    EXPECT_FALSE(backend.isOpen());
    EXPECT_TRUE(fileBytes() == before);

}

TEST_F(ScanReadProofTest, ColumnWindowReauthenticatesAfterWriteAndRejectsChangedFrameBeforeValues) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    auto readMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    struct Hook final : graph::utils::UnmanagedAllocationResource {
        std::shared_ptr<graph::query::execution::MaterializedByteBudget> budget;
        std::function<void()> onReserve;
        explicit Hook(decltype(budget) value) : budget(std::move(value)) {}
        void reserveUnmanaged(size_t bytes) override {
            budget->reserveUnmanaged(bytes);
            try { if (onReserve) onReserve(); }
            catch (...) { budget->releaseUnmanaged(bytes); throw; }
        }
        void releaseUnmanaged(size_t bytes) noexcept override { budget->releaseUnmanaged(bytes); }
        void *do_allocate(size_t bytes, size_t alignment) override { return budget->allocate(bytes, alignment); }
        void do_deallocate(void *p, size_t bytes, size_t alignment) override { budget->deallocate(p, bytes, alignment); }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
    };
    auto hook = std::make_shared<Hook>(readMemory);
    graph::utils::OwnedAllocationScope readOwner(hook);
    auto pin = store().openSnapshot();
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore large(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages, large, store());
    BTreePageStore trees(pages, store());
    const auto desc = targetDescriptor(pin);
    const auto binding = HybridPropertyDirectoryCodec::bindRoot(desc, pin, HybridPropertyRootOrdinal::HPRO_NODE_GROUP);
    HybridPropertyGroupDirectoryMap groups(binding.root.index_id, trees, store(), binding);
    const auto locator = groups.find(pin, HybridPropertyDirectoryCodec::groupKey(1)); ASSERT_TRUE(locator);
    const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*locator);
    const auto location = store().findPageLocation(pin, slot.page_id);
    const auto pool = store().findCatalog(pin, location.segment_id);
    const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(pin.superblock().data_region_offset, pool.physical_slot) +
        uint64_t(location.page_index) * RSS_PAGE_SIZE + RSS_PAGE_SIZE - 1U;
    std::byte original{}; ASSERT_EQ(targetIo()->readAt(offset, &original, 1U), 1U);
    size_t reuses = 0, delivered = 0; bool changed = false;
    graph::debug::PerfTrace::ScopedObserver observer(&reuses,
        [](void *p, std::string_view key, int64_t value, bool duration) noexcept {
            if (!duration && key == "native_graph.hybrid.column_window_page_reuses")
                *static_cast<size_t *>(p) += size_t(value);
        });
    hook->onReserve = [&] {
        if (reuses && !changed) {
            const auto bad = original ^ std::byte{1}; targetIo()->writeAt(offset, &bad, 1U); changed = true;
        }
    };
    std::array<EntityPropertyKey, 64> owners{};
    for (size_t i = 0; i < owners.size(); ++i) owners[i] = {EntityPropertyOwnerKind::EPOK_NODE, i + 1U};
    const uint32_t token = 1;
    const PropertyScalarReadSink scalar = [&](auto, auto, auto, auto) { ++delivered; };
    const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> values = [](auto, auto, auto) {};
    const PropertyTextReadSink text = [](auto, auto, auto) {};
    const PropertyColumnBatchReadSinks batches{
        [&](auto rows, auto, auto, auto) { delivered += rows.size(); }, [](auto, auto, auto) {}};
    auto cursor = properties.openScanCursor(pin);
    EXPECT_THROW(cursor.readEntityPropertiesOptionalSorted(pin, owners, std::span(&token, 1), {}, true,
        nullptr, &values, &scalar, &text, &batches), FormatException);
    EXPECT_TRUE(changed); EXPECT_GT(reuses, 0U); EXPECT_EQ(delivered, 0U);
    targetIo()->writeAt(offset, &original, 1U);
}

TEST_F(ScanReadProofTest, FieldReaderSkipsOnlyCertifiedOwnerRoutesAndNeverCachesVisitorAuthority) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    auto readMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    graph::utils::OwnedAllocationScope readOwner(readMemory);
    auto pin = store().openSnapshot();
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore large(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages, large, store());
    auto cursor = properties.openScanCursor(pin);
    HybridPropertyReadContext pointContext(targetIo(), pages, store(), pin);
    std::array<EntityPropertyKey, 64> owners{};
    for (size_t i = 0; i < owners.size(); ++i) owners[i] = {EntityPropertyOwnerKind::EPOK_NODE, i + 1U};
    struct Counts { int64_t actual = 0, saved = 0, page_reuses = 0; } counts;
    graph::debug::PerfTrace::ScopedObserver observer(&counts,
        [](void *p, std::string_view key, int64_t value, bool duration) noexcept {
            if (duration) return;
            auto &c = *static_cast<Counts *>(p);
            if (key == "native_graph.hybrid.owner_route_checks_performed") c.actual += value;
            if (key == "native_graph.hybrid.certified_owner_route_checks_saved") c.saved += value;
            if (key == "native_graph.hybrid.column_window_page_reuses") c.page_reuses += value;
        });
    size_t scalars = 0, cold = 0;
    const PropertyScalarReadSink scalar = [&](size_t row, size_t column, auto, uint64_t bits) {
        EXPECT_EQ(column, 0U); EXPECT_EQ(bits, row + 1U); ++scalars;
    };
    const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> values =
        [&](size_t row, size_t column, auto value) {
            if (value) {
                EXPECT_EQ(row, 0U); EXPECT_EQ(column, 0U);
                EXPECT_EQ(value->value, graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})}));
                ++cold;
            }
        };
    const PropertyTextReadSink text = [](auto, auto, auto) { ADD_FAILURE() << "unexpected text"; };
    const PropertyColumnBatchReadSinks batches{
        [&](auto rows, auto column, auto type, auto bits) {
            for (size_t i = 0; i < rows.size(); ++i) scalar(rows[i], column, type, bits[i]);
        }, [](auto, auto, auto) { ADD_FAILURE() << "unexpected text run"; }};
    const auto read = [&](uint32_t token) {
        cursor.readEntityPropertiesOptionalSorted(pin, owners, std::span(&token, 1), {}, true, nullptr,
            &values, &scalar, &text, &batches);
    };
    read(1); EXPECT_EQ(scalars, 64U);
    EXPECT_GT(counts.page_reuses, 0);
    EXPECT_GE(counts.actual, 64); EXPECT_EQ(counts.saved, 0); counts = {};
    auto rows = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    EXPECT_TRUE(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 512, {},
        [&](auto ids, size_t examined, const auto &grant) {
            EXPECT_EQ(ids.size(), 64U); EXPECT_EQ(examined, 64U);
            grant.read(ids, [&] {
                read(1); EXPECT_GE(counts.saved, 64); read(3);
                EXPECT_TRUE(pointContext.ownerLiveness(owners[0]).value_or(false));
            });
            return true;
        }));
    EXPECT_EQ(scalars, 128U); EXPECT_EQ(cold, 1U);
    EXPECT_EQ(counts.actual, 0); EXPECT_GE(counts.saved, 128); counts = {};
    read(1); EXPECT_EQ(scalars, 192U);
    EXPECT_GE(counts.actual, 64); EXPECT_EQ(counts.saved, 0); EXPECT_EQ(rows->used(), 0U);
    counts = {};
    EXPECT_TRUE(pointContext.ownerLiveness(owners[0]).value_or(false));
    EXPECT_EQ(counts.actual, 1); EXPECT_EQ(counts.saved, 0);
}

TEST_F(ScanReadProofTest, MultiFieldBatchSharesBoundedDirectoryReadsWithoutLeakingOwnerAuthority) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 2U, false, 512U));
    // A single raw frame forces borrowed data/metadata reads to coexist within
    // the established cache policy, rather than requiring a larger cache.
    targetIo()->enableBufferPool(RSS_PAGE_SIZE, work);
    auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    graph::utils::OwnedAllocationScope scope(memory);
    auto pin = store().openSnapshot();
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore large(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages, large, store());
    auto cursor = properties.openScanCursor(pin);
    std::array<EntityPropertyKey, 512> owners{};
    for (size_t i=0;i<owners.size();++i) owners[i]={EntityPropertyOwnerKind::EPOK_NODE,i+1U};
    const std::array<uint32_t,2> tokens{1U,2U};
    std::array<unsigned,512> numbers{},texts{};
    size_t numberRuns=0,textRuns=0;
    struct Counts { int64_t refills=0,hits=0,multi=0,actual=0,saved=0; } counts;
    graph::debug::PerfTrace::ScopedObserver observer(&counts,[](void *p,std::string_view key,int64_t value,bool duration) noexcept {
        if(duration)return;auto &c=*static_cast<Counts *>(p);
        if(key=="native_graph.hybrid.group_directory_window_refills")c.refills+=value;
        if(key=="native_graph.hybrid.group_directory_window_hits")c.hits+=value;
        if(key=="native_graph.hybrid.grouped_multi_column_batches")c.multi+=value;
        if(key=="native_graph.hybrid.owner_route_checks_performed")c.actual+=value;
        if(key=="native_graph.hybrid.certified_owner_route_checks_saved")c.saved+=value;
    });
    const std::function<void(size_t,size_t,std::optional<PropertyCollectionRootEntry>)> values=
        [](auto,auto,auto){ADD_FAILURE()<<"selected native fields unexpectedly used owning fallback";};
    const PropertyScalarReadSink scalar=[&](size_t row,size_t column,auto type,uint64_t word){
        ASSERT_LT(row,numbers.size());EXPECT_EQ(column,0U);EXPECT_EQ(type,graph::PropertyType::INTEGER);
        EXPECT_EQ(word,row+1U);++numbers[row];
    };
    const PropertyTextReadSink text=[&](size_t row,size_t column,std::string_view value){
        ASSERT_LT(row,texts.size());EXPECT_EQ(column,1U);EXPECT_EQ(value,"short");++texts[row];
    };
    const PropertyColumnBatchReadSinks batches{
        [&](auto rows,auto column,auto type,auto words){++numberRuns;for(size_t j=0;j<rows.size();++j)scalar(rows[j],column,type,words[j]);},
        [&](auto rows,auto column,auto words){++textRuns;for(size_t j=0;j<rows.size();++j)text(rows[j],column,words[j]);}};
    const auto read=[&]{cursor.readEntityPropertiesOptionalSorted(pin,owners,tokens,{},true,nullptr,&values,&scalar,&text,&batches);};
    auto rows=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U*1024U);
    ASSERT_TRUE(CertifiedScanMembership::visit(verified,targetIo(),store(),pin,rows,SIZE_MAX,512,{},
        [&](auto ids,size_t examined,const auto &grant){EXPECT_EQ(ids.size(),512U);EXPECT_EQ(examined,512U);grant.read(ids,read);return true;}));
    for(size_t i=0;i<owners.size();++i){EXPECT_EQ(numbers[i],1U);EXPECT_EQ(texts[i],1U);}
    EXPECT_EQ(numberRuns,8U);EXPECT_EQ(textRuns,8U);EXPECT_EQ(counts.multi,8);
    EXPECT_EQ(counts.refills,1);EXPECT_GE(counts.hits,7);EXPECT_EQ(counts.actual,0);EXPECT_GE(counts.saved,512);
    // The same retained reader outside a producer grant must authenticate all
    // owners normally; cached directory locators are never visibility proof.
    counts={};numbers.fill(0);texts.fill(0);numberRuns=textRuns=0;
    ASSERT_NO_THROW(read());
    for(size_t i=0;i<owners.size();++i){EXPECT_EQ(numbers[i],1U);EXPECT_EQ(texts[i],1U);}
    EXPECT_EQ(counts.saved,0);EXPECT_GE(counts.actual,512);EXPECT_EQ(counts.refills,0);
}

TEST_F(ScanReadProofTest, MixedNativeAndColdFieldsDeclineBeforeAnyGroupedPrefixIsExposed) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true,true));
    auto memory=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U*1024U);
    graph::utils::OwnedAllocationScope scope(memory);
    auto pin=store().openSnapshot();
    SlottedPagePool pages(targetIo(),store(),SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore large(targetIo(),store(),SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages,large,store());auto cursor=properties.openScanCursor(pin);
    std::array<EntityPropertyKey,64> owners{};for(size_t i=0;i<owners.size();++i)owners[i]={EntityPropertyOwnerKind::EPOK_NODE,i+1U};
    const std::array<uint32_t,2> tokens{1U,3U};std::array<std::array<unsigned,2>,64> seen{};size_t runs=0;
    const PropertyScalarReadSink scalar=[&](size_t row,size_t column,auto type,uint64_t word){
        ASSERT_LT(row,seen.size());EXPECT_EQ(column,0U);EXPECT_EQ(type,graph::PropertyType::INTEGER);EXPECT_EQ(word,row+1U);++seen[row][column];};
    const PropertyTextReadSink text=[](auto,auto,auto){ADD_FAILURE()<<"unexpected text";};
    const std::function<void(size_t,size_t,std::optional<PropertyCollectionRootEntry>)> values=[&](size_t row,size_t column,auto value){
        ASSERT_LT(row,seen.size());ASSERT_EQ(column,1U);++seen[row][column];
        if(row==0U){ASSERT_TRUE(value);EXPECT_EQ(value->value,graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})}));}
        else EXPECT_FALSE(value);
    };
    const PropertyColumnBatchReadSinks batches{
        [&](auto rows,auto column,auto type,auto words){++runs;for(size_t j=0;j<rows.size();++j)scalar(rows[j],column,type,words[j]);},
        [](auto,auto,auto){ADD_FAILURE()<<"unexpected text run";}};
    cursor.readEntityPropertiesOptionalSorted(pin,owners,tokens,{},true,nullptr,&values,&scalar,&text,&batches);
    for(const auto &row:seen){EXPECT_EQ(row[0],1U);EXPECT_EQ(row[1],1U);}
    EXPECT_EQ(runs,0U);
}

TEST_F(ScanReadProofTest, DirectoryBatchWindowCannotKeepAProducerGrantAcrossAnIoEpochChange) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true,true,false,2U,false,512U));
    auto verified=VerifiedScanRoot::verifyComplete(targetIo(),store(),work,control);
    auto memory=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U*1024U);
    graph::utils::OwnedAllocationScope scope(memory);auto pin=store().openSnapshot();
    SlottedPagePool pages(targetIo(),store(),SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore large(targetIo(),store(),SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages,large,store());auto cursor=properties.openScanCursor(pin);
    std::array<EntityPropertyKey,512> owners{};for(size_t i=0;i<owners.size();++i)owners[i]={EntityPropertyOwnerKind::EPOK_NODE,i+1U};
    const std::array<uint32_t,2> tokens{1U,2U};std::array<std::array<unsigned,2>,512> seen{};
    struct Change {StorageIO *io;std::byte original{};bool changed=false,failed=false;} change{targetIo().get()};
    change.io->readAt(0,&change.original,1U);
    graph::debug::PerfTrace::ScopedObserver observer(&change,[](void *p,std::string_view key,int64_t,bool duration) noexcept {
        auto &c=*static_cast<Change *>(p);
        if(!duration && !c.changed && key=="native_graph.hybrid.group_directory_window_hits"){
            c.changed=true;try{c.io->writeAt(0,&c.original,1U);}catch(...){c.failed=true;}
        }
    });
    const std::function<void(size_t,size_t,std::optional<PropertyCollectionRootEntry>)> values=[](auto,auto,auto){ADD_FAILURE()<<"unexpected owning fallback";};
    const PropertyScalarReadSink scalar=[&](size_t row,size_t column,auto,uint64_t word){ASSERT_LT(row,seen.size());ASSERT_EQ(column,0U);EXPECT_EQ(word,row+1U);++seen[row][column];};
    const PropertyTextReadSink text=[&](size_t row,size_t column,std::string_view value){ASSERT_LT(row,seen.size());ASSERT_EQ(column,1U);EXPECT_EQ(value,"short");++seen[row][column];};
    const PropertyColumnBatchReadSinks batches{
        [&](auto rows,auto column,auto type,auto words){for(size_t j=0;j<rows.size();++j)scalar(rows[j],column,type,words[j]);},
        [&](auto rows,auto column,auto words){for(size_t j=0;j<rows.size();++j)text(rows[j],column,words[j]);}};
    const auto read=[&]{cursor.readEntityPropertiesOptionalSorted(pin,owners,tokens,{},true,nullptr,&values,&scalar,&text,&batches);};
    auto rows=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U*1024U);
    const auto visit=[&](const auto &proof){return CertifiedScanMembership::visit(proof,targetIo(),store(),pin,rows,SIZE_MAX,512,{},
        [&](auto ids,size_t,const auto &grant){grant.read(ids,read);return true;});};
    EXPECT_THROW(static_cast<void>(visit(verified)),FormatException);
    EXPECT_TRUE(change.changed);EXPECT_FALSE(change.failed);
    for(const auto &row:seen){EXPECT_LE(row[0],1U);EXPECT_LE(row[1],1U);}
    // Bytes and logical state did not change. A fresh complete verification
    // restores authority, while the expired proof above remains unusable.
    auto fresh=VerifiedScanRoot::verifyComplete(targetIo(),store(),work,control);seen.fill({});
    ASSERT_TRUE(visit(fresh));for(const auto &row:seen){EXPECT_EQ(row[0],1U);EXPECT_EQ(row[1],1U);}
}

TEST_F(ScanReadProofTest, CompletionLeavesManagedInsertionSummariesFreshAndActual) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true,true,false,2U,false,4096U));
    auto pin=store().openSnapshot();
    EXPECT_EQ(pin.superblock().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING,0U);
    SlottedPagePool pages(targetIo(),store(),SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore indexes(pages,store());EntityPageStore entities(targetIo(),store(),SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    const auto proof=EntityFreeSpaceIndex::readProof(store(),indexes,pin,EntityPageRecordKind::EPRK_NODE);
    ASSERT_TRUE(proof);ASSERT_TRUE(proof->fresh);
    size_t observed=0;
    EXPECT_TRUE(EntityFreeSpaceIndex::visitCandidates(store(),indexes,pin,EntityPageRecordKind::EPRK_NODE,*proof,
        size_t(proof->free_pages)+1U,[&](const auto &entry){
            ++observed;EXPECT_EQ(entities.freeSlotCount(pin,entry.page,EntityPageRecordKind::EPRK_NODE),entry.free_slots);
        }));
    EXPECT_EQ(observed,proof->free_pages);
}

TEST_F(ScanReadProofTest, BoundedDerivedSummaryMaintenancePreservesNativeFieldsAndOldPins) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true,true));builder.reset();
    RelocatableStorageBackendOptions options;options.data_path=target;options.open_mode=OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options);backend.open();auto old=backend.openLogicalGraphReadSnapshot();
    const auto before=backend.openSnapshot().superblock();
    auto oversized=std::make_shared<graph::query::execution::MaterializedByteBudget>(8U*1024U*1024U);
    EXPECT_THROW(backend.rebuildNativeEntityFreeSpaceSummaries({},oversized),std::invalid_argument);
    auto tiny=std::make_shared<graph::query::execution::MaterializedByteBudget>(1U);
    EXPECT_THROW(backend.rebuildNativeEntityFreeSpaceSummaries({},tiny),graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(backend.openSnapshot().superblock(),before);EXPECT_EQ(tiny->used(),0U);
    auto bounded=std::make_shared<graph::query::execution::MaterializedByteBudget>(7U*1024U*1024U);
    ASSERT_NO_THROW(backend.rebuildNativeEntityFreeSpaceSummaries({},bounded));
    EXPECT_EQ(old->readNodeProperty(1,"number"),graph::PropertyValue(int64_t{1}));
    EXPECT_EQ(old->readNodeProperty(1,"text"),graph::PropertyValue("short"));
    auto current=backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1,"number"),graph::PropertyValue(int64_t{1}));
    EXPECT_EQ(current->readNodeProperty(1,"text"),graph::PropertyValue("short"));
    old.reset();current.reset();backend.close();EXPECT_EQ(bounded->used(),0U);
    options.open_mode=OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend reopened(options);reopened.open();
    auto final=reopened.openLogicalGraphReadSnapshot();EXPECT_EQ(final->readNodeProperty(1,"number"),graph::PropertyValue(int64_t{1}));
    final.reset();reopened.close();
}


TEST_F(ScanReadProofTest, NativeRowFallbackAvoidsRebuildingUnrelatedFieldPoolsAndOwnerGroups) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 16U, false, 2048U, true));
    LogicalGraphMutationBatch batch;
    for (int64_t i = 0; i < 1000; ++i) batch.edge_creates.push_back({
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2},
        "LINK", {{"weight", graph::PropertyValue(i)}}});
    struct Counters { int64_t descriptors = 0, poolFrames = 0; } counters;
    NativeGraphMutationExecutionResult result;
    {
        auto caller = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
        graph::utils::OwnedAllocationScope owner(caller);
        graph::debug::PerfTrace::ScopedObserver observer(&counters,
            [](void *owner, std::string_view key, int64_t value, bool) noexcept {
                auto &counts = *static_cast<Counters *>(owner);
                if (key == "native_graph.read.metadata_decodes.hybrid_descriptor") counts.descriptors += value;
                if (key == "storage.slotted.free_index.pool_frames") counts.poolFrames += value;
            });
        ASSERT_NO_THROW(result = mutateImage(batch));
    }
    ASSERT_TRUE(result.committed()); ASSERT_EQ(result.result.created_edge_ids.size(), 1000U);
    EXPECT_LE(counters.descriptors, 64); // Source descriptors must not be reopened per new ROW owner.
    EXPECT_LE(counters.poolFrames, int64_t(RSS_PAGE_POOL_DATA_FRAME_COUNT)); // One compatible ROW pool already has space.
    builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend reopened(options); ASSERT_NO_THROW(reopened.open());
    auto snapshot = reopened.openLogicalGraphReadSnapshot();
    EXPECT_EQ(snapshot->readNodeProperty(1, "number"), graph::PropertyValue(int64_t{1}));
    EXPECT_EQ(snapshot->readNodeProperty(2048, "K16"), graph::PropertyValue(int64_t{2063}));
    size_t seen = 0;
    for (size_t i = 0; i < result.result.created_edge_ids.size(); ++i) {
        EXPECT_EQ(snapshot->readEdgeProperty(result.result.created_edge_ids[i], "weight"), graph::PropertyValue(int64_t(i)));
        ++seen;
    }
    EXPECT_EQ(seen, 1000U);
    snapshot.reset(); EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity())); reopened.close();
}

TEST_F(ScanReadProofTest, NativeRowPlacementStopsAfterAnAuthenticatedCompatiblePool) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 16U, false, 2048U, true));
    auto pin = store().openSnapshot();
    const auto seeded = store().findEntityPropertyLocationOptional(pin, {EntityPropertyOwnerKind::EPOK_EDGE, 1});
    ASSERT_TRUE(seeded);
    const auto row = store().findPageLocation(pin, seeded->page_id);
    std::vector<SegmentId> candidates{row.segment_id};
    store().visitCatalogEntries(pin, [&](const CatalogEntry &pool) {
        if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && (pool.state == SegmentState::SGS_BUILDING || pool.state == SegmentState::SGS_SEALED) &&
                pool.segment_id != row.segment_id) candidates.push_back(pool.segment_id);
    });
    ASSERT_GT(candidates.size(), 1U);
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    const auto selectedPoolUpperBound = int64_t(RSS_PAGE_POOL_DATA_FRAME_COUNT - pages.availableFrameCount(pin, row.segment_id));
    int64_t frames = 0;
    std::optional<SlottedPagePlacement> selected;
    {
        graph::debug::PerfTrace::ScopedObserver observer(&frames,
            [](void *owner, std::string_view key, int64_t value, bool) noexcept {
                if (key == "storage.slotted.free_index.pool_frames") *static_cast<int64_t *>(owner) += value;
            });
        selected = pages.findBestFitPageForRecord(pin, candidates, PageOwnerKind::POK_PROPERTY, 32U);
    }
    ASSERT_TRUE(selected); EXPECT_EQ(selected->segment_id, row.segment_id);
    EXPECT_LE(frames, selectedPoolUpperBound);
    candidates.push_back(SegmentId{});
    EXPECT_THROW(static_cast<void>(pages.findBestFitPageForRecord(pin, candidates,
        PageOwnerKind::POK_PROPERTY, 32U)), FormatException);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
}

TEST_F(ScanReadProofTest, NativeGroupAbsenceDoesNotRetagMixedNodeAndEdgeWrites) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 2U, false, 64U, true));
    LogicalGraphMutationBatch batch;
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {{"number", graph::PropertyValue(int64_t{999})}}});
    for (int64_t i = 0; i < 32; ++i) batch.edge_creates.push_back({
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2},
        "LINK", {{"weight", graph::PropertyValue(i)}}});
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope scope(memory);
    struct Change { StorageIO *io; std::byte original{}; bool changed = false, failed = false; } change{targetIo().get()};
    change.io->readAt(0, &change.original, 1U);
    NativeGraphMutationExecutionResult result;
    {
        graph::debug::PerfTrace::ScopedObserver observer(&change,
            [](void *owner, std::string_view key, int64_t, bool duration) noexcept {
                auto &c = *static_cast<Change *>(owner);
                if (!duration && !c.changed && key == "native_graph.write.group_absence_cache_hits") {
                    c.changed = true;
                    try { c.io->writeAt(0, &c.original, 1U); } catch (...) { c.failed = true; }
                }
            });
        result = mutateImage(batch);
    }
    EXPECT_TRUE(change.changed); EXPECT_FALSE(change.failed); ASSERT_TRUE(result.committed());
    ASSERT_EQ(result.result.created_edge_ids.size(), 32U);
    builder.reset();
    RelocatableStorageBackendOptions options; options.data_path = target;
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend reopened(options); ASSERT_NO_THROW(reopened.open());
    auto snapshot = reopened.openLogicalGraphReadSnapshot();
    EXPECT_EQ(snapshot->readNodeProperty(1, "number"), graph::PropertyValue(int64_t{999}));
    EXPECT_EQ(snapshot->readNodeProperty(2, "number"), graph::PropertyValue(int64_t{2}));
    for (size_t i = 0; i < result.result.created_edge_ids.size(); ++i)
        EXPECT_EQ(snapshot->readEdgeProperty(result.result.created_edge_ids[i], "weight"), graph::PropertyValue(int64_t(i)));
    snapshot.reset(); EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity())); reopened.close();
}

TEST_F(ScanReadProofTest, NativePoolHintFallsBackWhenAnObserverChangesTheIoEpoch) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 16U, false, 2048U, true));
    auto pin = store().openSnapshot();
    const auto seeded = store().findEntityPropertyLocationOptional(pin, {EntityPropertyOwnerKind::EPOK_EDGE, 1});
    ASSERT_TRUE(seeded);
    const auto row = store().findPageLocation(pin, seeded->page_id);
    std::vector<SegmentId> candidates;
    store().visitCatalogEntries(pin, [&](const CatalogEntry &pool) {
        if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL &&
            (pool.state == SegmentState::SGS_BUILDING || pool.state == SegmentState::SGS_SEALED) &&
            pool.segment_id != row.segment_id) candidates.push_back(pool.segment_id);
    });
    candidates.push_back(row.segment_id);
    struct Change { StorageIO *io; std::byte original{}; bool changed = false, failed = false; } change{targetIo().get()};
    change.io->readAt(0, &change.original, 1U);
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    {
        graph::debug::PerfTrace::ScopedObserver observer(&change,
            [](void *owner, std::string_view key, int64_t, bool duration) noexcept {
                auto &c = *static_cast<Change *>(owner);
                if (!duration && !c.changed && key == "storage.slotted.native_row_pool_hints") {
                    c.changed = true;
                    try { c.io->writeAt(0, &c.original, 1U); } catch (...) { c.failed = true; }
                }
            });
        const auto selected = pages.findBestFitPageForRecord(pin, candidates, PageOwnerKind::POK_PROPERTY, 32U);
        ASSERT_TRUE(selected); EXPECT_EQ(selected->segment_id, row.segment_id);
    }
    EXPECT_TRUE(change.changed); EXPECT_FALSE(change.failed);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
}

TEST_F(ScanReadProofTest, ExhaustedNativeRowPoolUsesFreshCowDestinationAndPreservesOldPin) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 2U, false, 64U, true));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope scope(memory);
    auto old = store().openSnapshot();
    const auto original = store().findEntityPropertyLocationOptional(old, {EntityPropertyOwnerKind::EPOK_EDGE, 1});
    ASSERT_TRUE(original);
    for (int64_t value = 1; value <= 40; ++value) {
        LogicalGraphMutationBatch batch;
        batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_EDGE,
            LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {{"weight", graph::PropertyValue(value)}}});
        ASSERT_TRUE(mutateImage(batch).committed());
    }
    auto current = store().openSnapshot();
    const auto final = store().findEntityPropertyLocationOptional(current, {EntityPropertyOwnerKind::EPOK_EDGE, 1});
    ASSERT_TRUE(final);
    EXPECT_EQ(store().findEntityPropertyLocationOptional(old, {EntityPropertyOwnerKind::EPOK_EDGE, 1}), original);
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore values(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages, values, store());
    const auto before = properties.readEntityOptional(old, {EntityPropertyOwnerKind::EPOK_EDGE, 1});
    const auto after = properties.readEntityOptional(current, {EntityPropertyOwnerKind::EPOK_EDGE, 1});
    ASSERT_TRUE(before); ASSERT_TRUE(after); ASSERT_EQ(before->entries.size(), 1U); ASSERT_EQ(after->entries.size(), 1U);
    EXPECT_EQ(before->entries.front().value, graph::PropertyValue(int64_t{0}));
    EXPECT_EQ(after->entries.front().value, graph::PropertyValue(int64_t{40}));
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo(), store(), work).scan()));
}

TEST_F(ScanReadProofTest, PresenceProbesReuseSnapshotContextAndDropItAfterEpochChange) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto pin = store().openSnapshot();
        SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore large(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore properties(pages, large, store());
        auto cursor = properties.openScanCursor(pin);
        int64_t descriptors = 0;
        graph::debug::PerfTrace::ScopedObserver observer(&descriptors,
            [](void *owner, std::string_view name, int64_t value, bool duration) noexcept {
                if (!duration && name == "native_graph.read.metadata_decodes.hybrid_descriptor")
                    *static_cast<int64_t *>(owner) += value;
            });
        for (uint64_t id = 1; id <= 64; ++id) {
            const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, id};
            EXPECT_TRUE(cursor.readEntityPropertyExists(pin, owner, 1U, true));
            EXPECT_TRUE(cursor.readEntityPropertyExists(pin, owner, 2U, true));
            EXPECT_EQ(cursor.readEntityPropertyExists(pin, owner, 3U, true), id == 1U);
        }
        EXPECT_EQ(descriptors, 1);
        std::byte original{}; targetIo()->readAt(0, &original, 1U);
        targetIo()->writeAt(0, &original, 1U);
        EXPECT_TRUE(cursor.readEntityPropertyExists(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1U}, 1U, true));
        EXPECT_EQ(descriptors, 2);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanReadProofTest, QueryDirectoryContinuationSurvivesExecutionBatchesAndRestartsBackwardReads) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 2U, false, 2048U));
    targetIo()->enableBufferPool(1024U * 1024U, work);
    auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto rows = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto pin = store().openSnapshot();
        SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore large(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore properties(pages, large, store());
        auto cursor = properties.openScanCursor(pin);
        struct Counts { int64_t restarts = 0, refills = 0; } counts;
        graph::debug::PerfTrace::ScopedObserver observer(&counts,
            [](void *state, std::string_view name, int64_t value, bool duration) noexcept {
                if (duration) return;
                auto &c = *static_cast<Counts *>(state);
                if (name == "native_graph.hybrid.group_directory_cursor_restarts") c.restarts += value;
                if (name == "native_graph.hybrid.group_directory_window_refills") c.refills += value;
            });
        std::array<unsigned, 2048> seen{};
        const std::array<uint32_t, 1> tokens{1U};
        for (unsigned pass = 0; pass < 2; ++pass) {
            ASSERT_TRUE(CertifiedScanMembership::visit(verified, targetIo(), store(), pin, rows, SIZE_MAX, 256, {},
                [&](auto ids, size_t, const auto &grant) {
                    std::vector<EntityPropertyKey> owners;
                    for (auto id : ids) owners.push_back({EntityPropertyOwnerKind::EPOK_NODE, id});
                    auto alive = std::make_shared<bool>(true);
                    const SelectedPropertyReadAdmission admission = [alive](size_t) { EXPECT_TRUE(*alive); };
                    const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
                        [](auto, auto, auto) { ADD_FAILURE() << "unexpected owning scalar fallback"; };
                    const PropertyScalarReadSink scalar = [&](size_t row, size_t column, auto type, uint64_t value) {
                        ASSERT_LT(row, ids.size()); EXPECT_EQ(column, 0U);
                        EXPECT_EQ(type, graph::PropertyType::INTEGER); EXPECT_EQ(value, ids[row]);
                        ASSERT_LE(ids[row], seen.size()); ++seen[ids[row] - 1U];
                    };
                    const PropertyTextReadSink text = [](auto, auto, auto) { ADD_FAILURE() << "unexpected text"; };
                    const PropertyColumnBatchReadSinks batches{
                        [&](auto ordinals, auto column, auto type, auto values) {
                            for (size_t n = 0; n < ordinals.size(); ++n) scalar(ordinals[n], column, type, values[n]);
                        }, [](auto, auto, auto) { ADD_FAILURE() << "unexpected text run"; }};
                    grant.read(ids, [&] {
                        cursor.readEntityPropertiesOptionalSorted(pin, owners, tokens, {}, true,
                            &admission, &owning, &scalar, &text, &batches);
                    });
                    *alive = false;
                    return true;
                }));
            for (auto count : seen) EXPECT_EQ(count, pass + 1U);
            // Eight execution batches share one storage traversal, then a new
            // lower bound on the second pass deliberately restarts that stream.
            EXPECT_EQ(counts.restarts, pass + 1U);
            EXPECT_EQ(counts.refills, 4U * (pass + 1U));
            EXPECT_EQ(rows->used(), 0U);
        }
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanReadProofTest, QueryDirectoryContinuationDropsExpiredStateBeforeAFreshProducerGrant) {
    ASSERT_NO_FATAL_FAILURE(makeImage(true, true, false, 2U, false, 1024U));
    auto verified = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto rows = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    graph::utils::OwnedAllocationScope scope(memory);
    auto pin = store().openSnapshot();
    SlottedPagePool pages(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore large(targetIo(), store(), SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(pages, large, store()); auto cursor = properties.openScanCursor(pin);
    std::array<unsigned, 1024> seen{}; const std::array<uint32_t, 1> tokens{1U};
    bool alterEpoch = true;
    const auto visit = [&](const auto &proof) {
        return CertifiedScanMembership::visit(proof, targetIo(), store(), pin, rows, SIZE_MAX, 512, {},
            [&](auto ids, size_t, const auto &grant) {
                std::vector<EntityPropertyKey> owners;
                for (auto id : ids) owners.push_back({EntityPropertyOwnerKind::EPOK_NODE, id});
                const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
                    [](auto, auto, auto) { ADD_FAILURE() << "unexpected owning fallback"; };
                const PropertyScalarReadSink scalar = [&](size_t row, size_t column, auto, uint64_t value) {
                    EXPECT_EQ(column, 0U); ASSERT_LT(row, ids.size()); EXPECT_EQ(value, ids[row]); ++seen[ids[row] - 1U];
                };
                const PropertyTextReadSink text = [](auto, auto, auto) { ADD_FAILURE() << "unexpected text"; };
                const PropertyColumnBatchReadSinks batches{
                    [&](auto ordinals, auto column, auto type, auto values) {
                        for (size_t n = 0; n < ordinals.size(); ++n) scalar(ordinals[n], column, type, values[n]);
                    }, [](auto, auto, auto) { ADD_FAILURE() << "unexpected text run"; }};
                grant.read(ids, [&] { cursor.readEntityPropertiesOptionalSorted(pin, owners, tokens, {}, true,
                    nullptr, &owning, &scalar, &text, &batches); });
                if (alterEpoch) { std::byte byte{}; targetIo()->readAt(0, &byte, 1); targetIo()->writeAt(0, &byte, 1); }
                return true;
            });
    };
    EXPECT_THROW(static_cast<void>(visit(verified)), FormatException);
    for (size_t i = 0; i < seen.size(); ++i) EXPECT_EQ(seen[i], i < 512U ? 1U : 0U);
    alterEpoch = false; seen.fill(0);
    auto fresh = VerifiedScanRoot::verifyComplete(targetIo(), store(), work, control);
    ASSERT_TRUE(visit(fresh));
    for (auto count : seen) EXPECT_EQ(count, 1U);
    EXPECT_EQ(rows->used(), 0U);
}

#else
TEST(ScanReadProofTest, OrdinaryFormatsRejectVerificationBeforeIo) {
    const auto path = std::filesystem::temp_directory_path() /
        ("zyx-scan-proof-refusal-" + boost::uuids::to_string(boost::uuids::random_generator()()));
    {
        auto file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        Superblock root; root.database_id = DatabaseId(71); root.generation = Generation(1); root.checkpoint_lsn = LogSequenceNumber(1);
        auto metadata = RelocatableMetadataStore::initializeNew(io, root); io->resetStats();
        EXPECT_THROW(static_cast<void>(VerifiedScanRoot::verifyComplete({}, *metadata, {}, {})), FormatException);
        EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U);
    }
    std::error_code ignored; std::filesystem::remove(path, ignored);
}

#endif
