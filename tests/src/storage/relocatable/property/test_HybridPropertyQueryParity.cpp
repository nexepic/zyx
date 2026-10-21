#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "CypherParserImpl.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/adjacency/CoveredAdjacencyCandidate.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/LogicalGraphTransactionMemoryProvider.hpp"
#include <iostream>
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/maintenance/NativeStorageAllocationCensus.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"

using namespace graph;
using namespace graph::storage;
namespace {
// Test-only transaction: the real overlay owns staged final states. It cannot
// publish to the read-only file and carries the same retained private account.
class ReadControlOverlayTransaction final : public LogicalGraphTransaction, public LogicalGraphTransactionMemoryProvider {
    std::unique_ptr<LogicalGraphReadSnapshot> source_;
    std::shared_ptr<std::pmr::memory_resource> memory_;
    uint64_t next_node_ = 1000, next_edge_ = 10000; bool active_ = true;
public:
    ReadControlOverlayTransaction(std::unique_ptr<LogicalGraphReadSnapshot> source, std::shared_ptr<std::pmr::memory_resource> memory)
        : source_(std::move(source)), memory_(std::move(memory)) {}
    LogicalGraphTransactionState state() const override { return active_ ? LogicalGraphTransactionState::LGTTS_ACTIVE : LogicalGraphTransactionState::LGTTS_ROLLED_BACK; }
    bool isReadOnly() const override { return false; }
    LogicalGraphReadSnapshot &readSnapshot() override { return *source_; }
    std::shared_ptr<std::pmr::memory_resource> retainedWriteMemory() const override { return memory_; }
    LogicalGraphMutationBatchResult previewMutationBatch(const LogicalGraphMutationBatch &batch) const override {
        if (!active_) throw std::logic_error("test overlay transaction has ended");
        LogicalGraphMutationBatchResult result;
        for (size_t i = 0; i < batch.node_creates.size(); ++i) result.created_node_ids.push_back(next_node_ + i);
        for (size_t i = 0; i < batch.edge_creates.size(); ++i) result.created_edge_ids.push_back(next_edge_ + i);
        return result;
    }
    LogicalGraphMutationBatchResult stageMutationBatch(LogicalGraphMutationBatch batch) override {
        auto result = previewMutationBatch(batch); next_node_ += batch.node_creates.size(); next_edge_ += batch.edge_creates.size(); return result;
    }
    LogicalGraphMutationBatchResult commit() override { throw std::logic_error("read control overlay cannot publish"); }
    void rollback() override { active_ = false; }
};
class HybridPropertyQueryParityTest : public relocatable_storage_backend_test::RelocatableStorageBackendTest {
protected:
    using Rows = std::vector<std::vector<PropertyValue>>;
    const std::array<std::string, 7> queries{{
        "MATCH (n:Person) WHERE n.age >= 20 AND n.age < 25 RETURN n.age AS age ORDER BY age",
        "MATCH (n:Person) RETURN count(n) AS count, sum(n.age) AS total",
        "MATCH (n:Person) WHERE n.age = 7 RETURN n.note AS note, n.missing AS missing",
        "MATCH (n:Person) WHERE n.age < 3 OPTIONAL MATCH (n)-[r:KNOWS]->(m) RETURN n.age AS age, count(r) AS edges ORDER BY age",
        "MATCH (n)-[r:KNOWS]->(m) WHERE n.age < 4 RETURN n.age AS source, r.weight AS weight, m.age AS target ORDER BY source, weight, target",
        "MATCH (n:Person) RETURN n.age AS age ORDER BY age DESC LIMIT 3",
        "MATCH (n)-[r:KNOWS]->(m) RETURN r.weight AS weight, m.age AS age, m.note AS note, m.missing AS missing ORDER BY weight"
    }};
    void seed(bool propertyIndex = true, bool constraint = false, bool firstSolo = false, bool interleavedEdges = false, size_t edgeCount = 64, bool hotSource = false, bool mixedUntyped = false) {
        materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        std::vector<LogicalGraphNodeCreateRequest> nodes;
        for (int64_t i = 0; i < 64; ++i)
            nodes.push_back({{firstSolo && i == 0 ? "Solo" : "Person"}, {{"age", PropertyValue(i)}, {"note", PropertyValue(i == 7 ? "seven" : "person")}}});
        const auto ids = backend.createLogicalNodes(nodes); ASSERT_EQ(ids.size(), 64U);
        std::vector<LogicalGraphEdgeCreateRequest> edges;
        for (size_t i = 0; i < edgeCount; ++i)
            edges.push_back({ids[hotSource ? 0U : i / 2U], ids[interleavedEdges ? ((i % 32U) * 13U) % 32U : (i / 2U + 1U) % ids.size()], mixedUntyped && i % 3U == 0U ? "" : "KNOWS", {{"weight", PropertyValue(int64_t(i))}}});
        ASSERT_EQ(backend.createLogicalEdges(edges).size(), edgeCount);
        if (propertyIndex) static_cast<void>(backend.createLogicalNodeIndex({"person_age", "Person", {"age"}}));
        if (constraint) static_cast<void>(backend.createLogicalConstraint({"person_age_required",
            LogicalGraphConstraintEntityKind::LGCEK_NODE, LogicalGraphConstraintKind::LGCK_NOT_NULL,
            "Person", {"age"}, LogicalGraphConstraintPropertyType::LGCPT_NONE}));
        backend.close();
    }
    std::array<Rows, 7> read() {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        query::execution::LogicalPlanReadRuntime runtime(backend);
        query::execution::LogicalPlanReadRuntimeBounds bounds;
        bounds.maximum_node_records = 128; bounds.maximum_edge_records = 256; bounds.maximum_edges_per_node = 128;
        bounds.maximum_node_page_records = 16; bounds.maximum_metadata_pages_per_node_page = 8192;
        bounds.maximum_adjacency_page_records = 16; bounds.maximum_property_entries = 16; bounds.maximum_rows = 1024; bounds.maximum_distinct_rows = 1024;
        parser::cypher::CypherParserImpl parser; std::array<Rows, 7> output;
        for (size_t i = 0; i < queries.size(); ++i) {
            auto plan = parser.parseToLogical(queries[i]);
            const auto result = runtime.executeToQueryResult(*plan.root, bounds);
            for (const auto &row : result.getRows()) {
                std::vector<PropertyValue> values;
                for (const auto &column : result.getColumns()) values.push_back(row.at(column).asPrimitive());
                output[i].push_back(std::move(values));
            }
        }
        backend.close(); return output;
    }
    static constexpr uint64_t COVERED_INDEX = 700;
    void buildCovered(bool removeOne = false, bool wrongNeighbor = false, size_t edgeCount = 64) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); EntityPageStore entities(io, *metadata);
        SlottedPagePool pages(io, *metadata); BTreePageStore trees(pages, *metadata);
        EntityLabelSetStore labels(io, *metadata, pages); PropertyCollectionRootStore roots(pages, *metadata);
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(180);
        options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(180), 902};
        NativeGraphWritePlanner planner(*metadata, entities, labels, roots, trees, options); auto tx = planner.beginTransaction();
        std::vector<BTreePageEntry> entries;
        for (uint64_t id = 1; id <= edgeCount; ++id) {
            const auto edge = entities.readEdge(tx.baseSnapshot(), id);
            for (const auto direction : {GlobalAdjacencyDirection::GAD_OUT, GlobalAdjacencyDirection::GAD_IN}) {
                if (removeOne && id == 1 && direction == GlobalAdjacencyDirection::GAD_IN) continue;
                auto record = CoveredAdjacencyCandidate::entry(direction, id, edge.source_node_id, edge.target_node_id, edge.type_id);
                if (wrongNeighbor && id == 1 && direction == GlobalAdjacencyDirection::GAD_OUT) LittleEndianCodec::writeU64(record.value, 0, 64);
                const auto encoded = CoveredAdjacencyCandidate::Map::encode(record.key, record.value);
                entries.push_back({std::vector<std::byte>(encoded.begin(), encoded.end()), 0});
            }
        }
        std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
        const OrderedBTreeEntrySource source = [&]() -> OrderedBTreeEntryCursor {
            return [&, ordinal = size_t{0}]() mutable -> std::optional<BTreePageEntry> {
                if (ordinal == entries.size()) return {}; return entries[ordinal++];
            };
        };
        const BulkBTreeBuildOptions build{PhysicalSlotId(40), PhysicalSlotId(180), COVERED_INDEX, entries.size(), 32, BTreeLeafValueLayout::BLVL_EMPTY};
        tx.stageScalarIndexCreate(trees, BulkBTreeBuilder::planNativeTree(trees, *metadata, tx.baseSnapshot(), COVERED_INDEX, source, build));
        ASSERT_TRUE(tx.commit().committed()); io->flushStream();
    }
    std::array<Rows, 7> controlRows(bool covered, std::shared_ptr<query::execution::MaterializedByteBudget> memory, bool boundExpansion = false, bool multiHop = false) {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        auto snapshot = covered ? backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, memory) : backend.openLogicalGraphReadSnapshot();
        query::execution::LogicalPlanReadRuntime runtime(backend);
        query::execution::LogicalPlanReadRuntimeBounds bounds;
        bounds.maximum_node_records = 128; bounds.maximum_edge_records = 256; bounds.maximum_edges_per_node = 128;
        bounds.maximum_node_page_records = 16; bounds.maximum_metadata_pages_per_node_page = 8192;
        bounds.maximum_adjacency_page_records = 16; bounds.maximum_property_entries = 16; bounds.maximum_rows = 1024; bounds.maximum_distinct_rows = 1024;
        const std::array<std::vector<std::string>, 7> columns{{{"age"}, {"count", "total"}, {"note", "missing"}, {"age", "edges"},
            {"source", "weight", "target"}, {"age"}, {"weight", "age", "note", "missing"}}};
        parser::cypher::CypherParserImpl parser; std::array<Rows, 7> output;
        for (size_t i = 0; i < queries.size(); ++i) {
            const auto text = multiHop && i == 6 ?
                "MATCH (n:Person) WHERE n.age = 0 WITH n MATCH (n)-[:KNOWS*2]->(m) RETURN 0 AS weight, m.age AS age, m.note AS note, m.missing AS missing ORDER BY age" : boundExpansion && i == 6 ?
                "MATCH (n:Person) WHERE n.age = 0 WITH n MATCH (n)-[r:KNOWS]->(m) RETURN r.weight AS weight, m.age AS age, m.note AS note, m.missing AS missing ORDER BY weight" : queries[i];
            auto plan = parser.parseToLogical(text);
            (void)runtime.executeWithSnapshot(*snapshot, *plan.root, bounds, [&](const auto &row) {
                std::vector<PropertyValue> values;
                for (const auto &column : columns[i]) values.push_back(std::get<PropertyValue>(row.values.at(column)));
                output[i].push_back(std::move(values)); return true;
            }, {});
        }
        snapshot.reset(); EXPECT_EQ(memory->used(), 0U); backend.close(); return output;
    }
    NativeStorageAllocationFootprint wholeCost() {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::openReadOnly(io); auto pin = metadata->openSnapshot();
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        const auto epoch = io->dataMutationEpoch(); const auto cost = NativeStorageAllocationCensus(io, *metadata, memory).inspect(pin);
        if (!cost || !cost->fullyAttributed()) throw std::runtime_error("incomplete whole allocation proof");
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io->dataMutationEpoch(), epoch); return *cost;
    }
    void addCatalogControl(bool unknown, size_t unusedTokens = 0) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io);
        EntityPageStore entities(io, *metadata); SlottedPagePool pages(io, *metadata);
        BTreePageStore trees(pages, *metadata); EntityLabelSetStore labels(io, *metadata, pages);
        PropertyCollectionRootStore roots(pages, *metadata); SystemCatalog catalog(trees, *metadata);
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(180);
        options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(180), 902};
        NativeGraphWritePlanner planner(*metadata, entities, labels, roots, trees, options);
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        for (size_t i = 0; i < unusedTokens; ++i) (void)session.ensurePropertyToken("unused_" + std::to_string(i));
        session.setConfiguration("read_hint", PropertyValue(int64_t(7))); session.finish();
        if (unknown) {
            const std::array<std::byte, 2> key{std::byte{254}, std::byte{1}};
            tx.stageScalarIndexUpsert(trees, RSS_SYSTEM_CATALOG_INDEX_ID, key, 1);
        }
        ASSERT_TRUE(tx.commit().committed()); io->flushStream();
    }
    void setAge(int64_t value, bool expectRefusal = false, bool eraseOwner = false) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file->is_open()); auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io);
        // Writable-open recovery may repair pool bitmaps before the first
        // writer. Complete it through an independently checked metadata-only
        // lease before asserting that schema refusal does no private IO.
        if (expectRefusal) (void)metadata->reserveNativeGraphLogicalIds(1, 0);
        auto old = metadata->openSnapshot();
        const uint64_t owner = eraseOwner ? 64 : 1;
        SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, *metadata); SystemCatalog catalog(trees, *metadata);
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto catalogRoot = metadata->findIndexRoot(old, RSS_SYSTEM_CATALOG_INDEX_ID);
        const auto catalogLocation = metadata->findPageLocation(old, catalogRoot.root_page_id);
        const auto statistics = catalog.readGraphCardinalityStatistics(old);
        PropertyCollectionRootStore roots(pages, *metadata); const auto beforeRoot = roots.readEntity(old, {EntityPropertyOwnerKind::EPOK_NODE, owner});
        std::vector<uint64_t> ageIndexes;
        for (const auto &definition : catalog.readIndexDefinitions(old, 128))
            if (definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE && definition.kind == IndexDefinitionKind::IDK_PROPERTY &&
                definition.property_names.size() == 1 && definition.property_names.front() == "age") ageIndexes.push_back(definition.index_id);
        const auto age = std::ranges::find(beforeRoot.entries, *token, &PropertyCollectionRootEntry::key_token);
        ASSERT_NE(age, beforeRoot.entries.end());
        const auto oldKey = IndexKeyCodec::encodeEntryKey(age->value, owner);
        const auto newKey = IndexKeyCodec::encodeEntryKey(PropertyValue(value), owner);
        const auto before = old.superblock(); const auto epoch = io->dataMutationEpoch();
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{
            {{EntityPropertyOwnerKind::EPOK_NODE, owner}, *token, value, false}}};
        const std::array<EntityPropertyKey, 1> deleted{{{EntityPropertyOwnerKind::EPOK_NODE, owner}}};
        NativeHybridPropertyColumnWrite change{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            eraseOwner ? std::span<const NativeHybridPropertyColumnReplacement>{} : std::span(cells),
            memory, PhysicalSlotId(40), PhysicalSlotId(180), 903, nullptr, true,
            eraseOwner ? std::span(deleted) : std::span<const EntityPropertyKey>{}};
        if (expectRefusal) {
            EXPECT_THROW((void)metadata->mutateHybridPropertyColumns(change, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
            EXPECT_EQ(metadata->openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        } else {
            EXPECT_NO_THROW((void)metadata->mutateHybridPropertyColumns(change, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
            auto current = metadata->openSnapshot();
            if (eraseOwner) {
                EXPECT_NE(metadata->findPageLocation(current, metadata->findIndexRoot(current, RSS_SYSTEM_CATALOG_INDEX_ID).root_page_id), catalogLocation);
                ASSERT_TRUE(statistics); const auto counts = catalog.readGraphCardinalityStatistics(current); ASSERT_TRUE(counts);
                EXPECT_EQ(counts->node_count, statistics->node_count - 1); EXPECT_EQ(counts->edge_count, statistics->edge_count);
                EXPECT_FALSE(metadata->findNodePageLocationOptional(current, owner));
            } else {
                EXPECT_EQ(metadata->findIndexRoot(current, RSS_SYSTEM_CATALOG_INDEX_ID), catalogRoot);
                EXPECT_EQ(catalog.readGraphCardinalityStatistics(current), statistics);
                const auto cell = roots.readEntityPropertyOptional(current, {EntityPropertyOwnerKind::EPOK_NODE, owner}, *token);
                ASSERT_TRUE(cell); EXPECT_EQ(cell->value, PropertyValue(value));
            }
            for (const auto index : ageIndexes) {
                BTreeIndex tree(index, trees, *metadata);
                if (eraseOwner) EXPECT_FALSE(tree.find(current, newKey));
                else EXPECT_EQ(tree.find(current, newKey), std::optional<uint64_t>(owner));
                if (eraseOwner || oldKey != newKey) EXPECT_FALSE(tree.find(current, oldKey));
                EXPECT_EQ(tree.find(old, oldKey), std::optional<uint64_t>(owner));
            }
        }
        EXPECT_EQ(roots.readEntity(old, {EntityPropertyOwnerKind::EPOK_NODE, owner}), beforeRoot);
        EXPECT_EQ(memory->used(), 0U); io->flushStream();
    }
    void convert(EntityPropertyOwnerKind kind, bool inverse = false) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file->is_open()); auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); const auto before = metadata->openSnapshot().superblock();
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        NativeGraphHybridPropertyConversion conversion{kind, 1, memory, nullptr, PhysicalSlotId(40), PhysicalSlotId(180), 903};
        conversion.publish_candidate = true;
        if (inverse) conversion.direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
        NativeGraphHybridPropertyRootMutation request{before.database_id, before.hybrid_property_descriptor_root,
            NativeHybridPropertyRootMode::NHPRM_REPLACE, {}, conversion};
        NativeGraphDerivedStateMutation derived;
        static_cast<void>(metadata->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived,
            LogSequenceNumber(before.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {},
            [](auto &) {}, {}, {}, {}, {}, {}, {}, {}, before.generation, {}, {}, {}, {}, {}, request));
        EXPECT_EQ(memory->used(), 0U); io->flushStream();
    }
};
}
TEST_F(HybridPropertyQueryParityTest, ExplicitReadOnlyCoveredControlUsesSameExecutorAndPreservesDuplicateGatherResults) {
    seed(true, false, false, true); buildCovered();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    const auto baseline = controlRows(false, memory, true); int64_t pages = 0;
    std::array<Rows, 7> candidate;
    {
        debug::PerfTrace::ScopedObserver observe(&pages, [](void *context, std::string_view key, int64_t value, bool) noexcept {
            if (key == "logical_graph.covered_candidate.reference_pages") *static_cast<int64_t *>(context) += value;
        });
        candidate = controlRows(true, memory, true);
    }
    EXPECT_EQ(candidate, baseline); EXPECT_GT(pages, 0);
    EXPECT_EQ(candidate[6], (Rows{{PropertyValue(int64_t(0)), PropertyValue(int64_t(0)), PropertyValue("person"), PropertyValue()},
        {PropertyValue(int64_t(1)), PropertyValue(int64_t(13)), PropertyValue("person"), PropertyValue()}}));
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, CoveredControlRefusesIncompleteCandidateInsteadOfFallingBack) {
    seed(); buildCovered(true); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    EXPECT_THROW((void)backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, memory), std::exception);
    EXPECT_EQ(memory->used(), 0U); backend.close(); EXPECT_EQ(read()[1][0][0], PropertyValue(int64_t(64)));
}
TEST_F(HybridPropertyQueryParityTest, CoveredControlRefusesCrcValidWrongEndpointAndReturnsItsBudget) {
    seed(); buildCovered(false, true); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    EXPECT_THROW((void)backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, memory), std::exception);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, CoveredControlAuthenticatesExactContinuationsAndReturnsAdmittedBuffers) {
    seed(); buildCovered(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    auto snapshot = backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, memory);
    query::execution::MaterializedByteReservation output(memory.get());
    const auto admission = [&](size_t bytes) { output.set(bytes); };
    auto first = snapshot->scanAdjacentEdgeRefsWithBufferAdmission(1, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1, admission);
    ASSERT_EQ(first.edges.size(), 1U); EXPECT_EQ(first.edges[0].entity_id, 1U); ASSERT_TRUE(first.continuation);
    EXPECT_EQ(output.bytes(), first.edges.capacity() * sizeof(EntityRef));
    const auto continuation = first.continuation; first = {}; output.reset();
    auto second = snapshot->scanAdjacentEdgeRefsWithBufferAdmission(1, LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 1, admission);
    ASSERT_EQ(second.edges.size(), 1U); EXPECT_EQ(second.edges[0].entity_id, 2U); EXPECT_FALSE(second.continuation);
    second = {}; output.reset();
    EXPECT_THROW((void)snapshot->scanAdjacentEdgeRefs(2, LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 1), std::exception);
    EXPECT_THROW((void)snapshot->scanAdjacentEdgeRefs(1, LogicalGraphReadDirection::LGRD_OUTGOING, LogicalGraphAdjacencyContinuation{2, 3}, 1), std::exception);
    EXPECT_THROW((void)snapshot->scanAdjacentEdgeRefs(1, LogicalGraphReadDirection::LGRD_OUTGOING, LogicalGraphAdjacencyContinuation{0, 1}, 1), std::invalid_argument);
    snapshot.reset(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, CoveredBatchKeepsOrderTotalBoundAndVisitorStopWithoutSourceBufferLeaks) {
    seed(); buildCovered(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    auto snapshot = backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, memory);
    query::execution::MaterializedByteReservation output(memory.get());
    const auto admission = [&](size_t bytes) { output.set(bytes); };
    const std::array<uint64_t, 2> nodes{1, 2}; std::vector<uint64_t> ids;
    snapshot->visitAdjacentEdgeRefsForNodesWithBufferAdmission(nodes, LogicalGraphReadDirection::LGRD_OUTGOING, 4,
        [&](const auto &edge) { ids.push_back(edge.entity_id); return true; }, admission);
    EXPECT_EQ(ids, (std::vector<uint64_t>{1, 2, 3, 4})); EXPECT_EQ(output.bytes(), 0U);
    size_t calls = 0;
    snapshot->visitAdjacentEdgeRefsForNodesWithBufferAdmission(nodes, LogicalGraphReadDirection::LGRD_OUTGOING, 4,
        [&](const auto &) { ++calls; return false; }, admission);
    EXPECT_EQ(calls, 1U); EXPECT_EQ(output.bytes(), 0U);
    const std::array<uint64_t, 2> duplicate{1, 1}; calls = 0;
    EXPECT_THROW(snapshot->visitAdjacentEdgeRefsForNodes(duplicate, LogicalGraphReadDirection::LGRD_OUTGOING, 4,
        [&](const auto &) { ++calls; return true; }), FormatException); EXPECT_EQ(calls, 0U);
    EXPECT_THROW(snapshot->visitAdjacentEdgeRefsForNodes(nodes, LogicalGraphReadDirection::LGRD_OUTGOING, 1,
        [&](const auto &) { return true; }), FormatException);
    snapshot.reset(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, CoveredControlSetupCancellationQuotaAndGraphBoundsRefundTheirAccounts) {
    seed(); buildCovered(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    struct Cancel final : CancellationCheckpoint { mutable size_t calls = 0; void check() const override { if (++calls == 10) throw std::runtime_error("covered setup cancelled"); } } cancellation;
    try { (void)backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, memory, &cancellation); FAIL(); }
    catch (const std::runtime_error &error) { EXPECT_EQ(std::string(error.what()), "covered setup cancelled"); }
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW((void)backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 63, 8192, memory), std::exception);
    EXPECT_EQ(memory->used(), 0U);
    auto tiny = std::make_shared<query::execution::MaterializedByteBudget>(65536);
    EXPECT_THROW((void)backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, tiny), query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    EXPECT_THROW((void)backend.openCoveredAdjacencyCandidateReadSnapshot(0, 64, 8192, memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, CoveredReadControlDoesNotGrantAuthorityToAWritableBackend) {
    seed(); buildCovered(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    EXPECT_THROW((void)backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, memory), std::invalid_argument);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, CoveredSupernodeUsesBoundedPagesAndExactLogicalContinuation) {
    seed(false, false, false, true, 90, true); buildCovered(false, false, 90);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    auto snapshot = backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 90, 8192, memory);
    auto first = snapshot->scanAdjacentEdgeRefs(1, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 80);
    ASSERT_EQ(first.edges.size(), 64U); ASSERT_TRUE(first.continuation);
    EXPECT_EQ(first.continuation->previous_edge_id, 64U); EXPECT_EQ(first.continuation->next_edge_id, 65U);
    auto second = snapshot->scanAdjacentEdgeRefs(1, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 80);
    ASSERT_EQ(second.edges.size(), 26U); EXPECT_FALSE(second.continuation);
    for (size_t i = 0; i < first.edges.size(); ++i) EXPECT_EQ(first.edges[i].entity_id, i + 1);
    for (size_t i = 0; i < second.edges.size(); ++i) EXPECT_EQ(second.edges[i].entity_id, i + 65);
    std::vector<uint64_t> ids; const std::array<uint64_t, 2> nodes{1, 2};
    snapshot->visitAdjacentEdgeRefsForNodes(nodes, LogicalGraphReadDirection::LGRD_OUTGOING, 90,
        [&](const auto &edge) { ids.push_back(edge.entity_id); return true; });
    ASSERT_EQ(ids.size(), 90U); for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], i + 1);
    snapshot.reset(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, CoveredEmptyGraphControlProvesAbsentTreeAndStillAuthenticatesTheNode) {
    seed(false, false, false, false, 0);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    auto snapshot = backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 1, 8192, memory);
    EXPECT_TRUE(snapshot->scanAdjacentEdgeRefs(1, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 64).edges.empty());
    EXPECT_THROW((void)snapshot->scanAdjacentEdgeRefs(999, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 64), FormatException);
    EXPECT_THROW((void)snapshot->scanAdjacentEdgeRefs(1, LogicalGraphReadDirection::LGRD_OUTGOING, LogicalGraphAdjacencyContinuation{1, 2}, 64), std::invalid_argument);
    snapshot.reset(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, CoveredPrototypeCountsBothCanonicalAndCandidatePoolsInWholeAllocation) {
    seed(false, false, false, true, 90, true); const auto baseline = wholeCost();
    buildCovered(false, false, 90); const auto candidate = wholeCost();
    const auto indexes = size_t(NativeStorageAllocationKind::NSAK_QUERY_INDEX);
    EXPECT_EQ(candidate.database_id, baseline.database_id);
    EXPECT_GE(candidate.sealed_pool_bytes[indexes], baseline.sealed_pool_bytes[indexes] + RSS_SEGMENT_SIZE);
    EXPECT_GE(candidate.steadyAllocatedBytes(), baseline.steadyAllocatedBytes() + RSS_SEGMENT_SIZE);
    EXPECT_GE(candidate.totalAllocatedBytes(), candidate.steadyAllocatedBytes() + candidate.retired_catalog_bytes);
    std::cout << "COVERED_CONTROL_COST edges=90 key_only=" << baseline.steadyAllocatedBytes()
        << " candidate=" << candidate.steadyAllocatedBytes() << " retired=" << candidate.retired_catalog_bytes
        << " allocated_peak=" << candidate.totalAllocatedBytes() << " file=" << candidate.file_bytes << '\n';
}
TEST_F(HybridPropertyQueryParityTest, CoveredReferenceControlPreservesRealOverlayDeletesCreatesAndPropertyChanges) {
    seed(true, false, false, true); buildCovered();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    const auto execute = [&](bool covered) {
        Rows result;
        {
            auto source = covered ? backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, memory) : backend.openLogicalGraphReadSnapshot();
            ReadControlOverlayTransaction transaction(std::move(source), memory);
            {
                LogicalGraphTransactionOverlay overlay(transaction);
                overlay.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(100))}, {"note", PropertyValue("overlay")}}}});
                overlay.deleteLogicalEdges({{1}});
                const auto created = overlay.createLogicalEdges({{1, 14, "KNOWS", {{"weight", PropertyValue(int64_t(99))}}}});
                EXPECT_EQ(created, (std::vector<uint64_t>{10000}));
                query::execution::LogicalPlanReadRuntime runtime(overlay); auto snapshot = overlay.openLogicalGraphReadSnapshot();
                parser::cypher::CypherParserImpl parser;
                auto plan = parser.parseToLogical("MATCH (n:Person) WHERE n.age = 100 WITH n MATCH (n)-[r:KNOWS]->(m) RETURN r.weight AS weight, m.age AS age ORDER BY weight");
                query::execution::LogicalPlanReadRuntimeBounds bounds;
                bounds.maximum_node_records = 128; bounds.maximum_edge_records = 256; bounds.maximum_edges_per_node = 128;
                bounds.maximum_node_page_records = 16; bounds.maximum_metadata_pages_per_node_page = 8192;
                bounds.maximum_adjacency_page_records = 16; bounds.maximum_property_entries = 16; bounds.maximum_rows = 1024; bounds.maximum_distinct_rows = 1024;
                (void)runtime.executeWithSnapshot(*snapshot, *plan.root, bounds, [&](const auto &row) {
                    result.push_back({std::get<PropertyValue>(row.values.at("weight")), std::get<PropertyValue>(row.values.at("age"))}); return true;
                }, {});
                EXPECT_THROW((void)transaction.commit(), std::logic_error);
            }
            transaction.rollback();
        }
        EXPECT_EQ(memory->used(), 0U); return result;
    };
    const auto baseline = execute(false); int64_t pages = 0; Rows candidate;
    {
        debug::PerfTrace::ScopedObserver observer(&pages, [](void *context, std::string_view key, int64_t value, bool) noexcept {
            if (key == "logical_graph.covered_candidate.reference_pages") *static_cast<int64_t *>(context) += value;
        });
        candidate = execute(true);
    }
    EXPECT_EQ(candidate, baseline); EXPECT_GT(pages, 0);
    EXPECT_EQ(candidate, (Rows{{PropertyValue(int64_t(1)), PropertyValue(int64_t(13))}, {PropertyValue(int64_t(99)), PropertyValue(int64_t(13))}}));
    backend.close(); EXPECT_EQ(read()[1], (Rows{{PropertyValue(int64_t(64)), PropertyValue(int64_t(2016))}}));
}
TEST_F(HybridPropertyQueryParityTest, CoveredTopologyAdapterPreservesUntypedNamesAndAdmittedBufferLifetime) {
    seed(false, false, false, false, 64, false, true); buildCovered();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    auto baseline = backend.openLogicalGraphReadSnapshot();
    auto candidate = backend.openCoveredAdjacencyCandidateReadSnapshot(COVERED_INDEX, 64, 8192, memory);
    const auto expected = baseline->scanAdjacentEdgeTopologies(1, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 2);
    query::execution::MaterializedByteReservation output(memory.get());
    auto actual = candidate->scanAdjacentEdgeTopologiesWithBufferAdmission(1, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 2,
        [&](size_t bytes) { output.set(bytes); });
    EXPECT_EQ(actual, expected); ASSERT_EQ(actual.edges.size(), 2U);
    EXPECT_TRUE(actual.edges[0].type.empty()); EXPECT_EQ(actual.edges[1].type, "KNOWS");
    size_t retained = actual.edges.capacity() * sizeof(LogicalGraphEdgeTopology);
    for (const auto &edge : actual.edges) retained += edge.type.capacity();
    EXPECT_EQ(output.bytes(), retained); actual = {}; output.reset();
    const auto used = memory->used();
    EXPECT_THROW((void)candidate->scanAdjacentEdgeTopologiesWithBufferAdmission(1, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 2,
        [&](size_t bytes) { if (bytes >= 2U * (sizeof(EntityRef) + sizeof(LogicalGraphEdgeTopology))) throw std::length_error("deny topology conversion"); }), std::length_error);
    EXPECT_EQ(memory->used(), used); candidate.reset(); baseline.reset(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, CoveredSameExecutorMultiHopRetainsAllParallelEdgePaths) {
    seed(); buildCovered(); auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    const auto baseline = controlRows(false, memory, false, true); int64_t pages = 0; std::array<Rows, 7> candidate;
    {
        debug::PerfTrace::ScopedObserver observer(&pages, [](void *context, std::string_view key, int64_t value, bool) noexcept {
            if (key == "logical_graph.covered_candidate.reference_pages") *static_cast<int64_t *>(context) += value;
        });
        candidate = controlRows(true, memory, false, true);
    }
    EXPECT_EQ(candidate, baseline); EXPECT_GT(pages, 0);
    EXPECT_EQ(candidate[6], (Rows(4, std::vector<PropertyValue>{PropertyValue(int64_t(0)), PropertyValue(int64_t(2)), PropertyValue("person"), PropertyValue()})));
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, DefaultProfileRemainsRow) {
#if !ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    seed(); const auto rows = read();
    EXPECT_EQ(rows[1], (Rows{{PropertyValue(int64_t(64)), PropertyValue(int64_t(2016))}}));
    EXPECT_THROW(convert(EntityPropertyOwnerKind::EPOK_NODE), FormatException);
    EXPECT_EQ(read(), rows);
#endif
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyQueryParityTest, SameExecutorPreservesIndexedFilterAggregateOptionalMultiedgesAndLimitAcrossBothLayouts) {
    seed(); const auto row = read();
    ASSERT_EQ(row[1], (Rows{{PropertyValue(int64_t(64)), PropertyValue(int64_t(2016))}}));
    ASSERT_EQ(row[0].size(), 5U); ASSERT_EQ(row[4].size(), 8U); ASSERT_EQ(row[5].size(), 3U);
    convert(EntityPropertyOwnerKind::EPOK_NODE); EXPECT_EQ(read(), row);
    convert(EntityPropertyOwnerKind::EPOK_EDGE); EXPECT_EQ(read(), row);
    convert(EntityPropertyOwnerKind::EPOK_NODE, true); EXPECT_EQ(read(), row);
    convert(EntityPropertyOwnerKind::EPOK_EDGE, true); EXPECT_EQ(read(), row);
}
TEST_F(HybridPropertyQueryParityTest, FieldUpdatesPreserveNonPropertyCatalogStateAndSameExecutorResults) {
    seed(false); addCatalogControl(false); const auto baseline = read(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    setAge(100); auto expected = baseline;
    expected[1] = Rows{{PropertyValue(int64_t(64)), PropertyValue(int64_t(2116))}};
    for (size_t query : {size_t(3), size_t(4)})
        std::erase_if(expected[query], [](const auto &row) { return row[0] == PropertyValue(int64_t(0)); });
    expected[5] = Rows{{PropertyValue(int64_t(100))}, {PropertyValue(int64_t(63))}, {PropertyValue(int64_t(62))}};
    EXPECT_EQ(read(), expected);
    setAge(0); EXPECT_EQ(read(), baseline);
}
TEST_F(HybridPropertyQueryParityTest, ScalarIndexAndNativeFieldPublishTogetherWithOldPinAndSameExecutorResults) {
    seed(); const auto baseline = read(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    setAge(100); const auto changed = read();
    EXPECT_EQ(changed[1], (Rows{{PropertyValue(int64_t(64)), PropertyValue(int64_t(2116))}}));
    EXPECT_EQ(changed[5], (Rows{{PropertyValue(int64_t(100))}, {PropertyValue(int64_t(63))}, {PropertyValue(int64_t(62))}}));
    setAge(0); EXPECT_EQ(read(), baseline);
}
TEST_F(HybridPropertyQueryParityTest, NonNullConstraintAllowsValidFieldPublication) {
    seed(false, true); const auto baseline = read(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    setAge(100); setAge(0); EXPECT_EQ(read(), baseline);
}
TEST_F(HybridPropertyQueryParityTest, IsolatedOwnerDeletionCoordinatesCatalogCountsAndLabelPropertyIndexes) {
    seed(true, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    setAge(100, false, true);
    EXPECT_EQ(read()[1], (Rows{{PropertyValue(int64_t(63)), PropertyValue(int64_t(1953))}}));
}
TEST_F(HybridPropertyQueryParityTest, UnknownCatalogControlNamespaceCannotBypassFieldCoordination) {
    seed(false); addCatalogControl(true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    setAge(100, true);
}
TEST_F(HybridPropertyQueryParityTest, UnrelatedTokenDictionarySizeIsNotAFieldMutationLifetimeCap) {
    seed(false); addCatalogControl(false, 4097); convert(EntityPropertyOwnerKind::EPOK_NODE);
    setAge(100); const auto result = read();
    EXPECT_EQ(result[1], (Rows{{PropertyValue(int64_t(64)), PropertyValue(int64_t(2116))}}));
}

namespace {
class HybridPropertyIndexedLifecycleTest : public HybridPropertyQueryParityTest {
protected:
    void compositeIndex() {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        (void)backend.createLogicalNodeIndex({"person_age_note", "Person", {"age", "note"}}); backend.close();
    }
    void edgeIndex() {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); EntityPageStore entities(io, *metadata);
        SlottedPagePool pages(io, *metadata); BTreePageStore trees(pages, *metadata);
        EntityLabelSetStore labels(io, *metadata, pages); PropertyCollectionRootStore roots(pages, *metadata);
        SystemCatalog catalog(trees, *metadata);
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(180);
        options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(180), 902};
        NativeGraphWritePlanner planner(*metadata, entities, labels, roots, trees, options);
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        IndexDefinition definition; definition.name = "edge_weight"; definition.entity_kind = IndexDefinitionEntityKind::IDEK_EDGE;
        definition.kind = IndexDefinitionKind::IDK_PROPERTY; definition.property_names = {"weight"};
        definition = session.createIndexDefinition(definition); session.finish();
        std::vector<BTreePageEntry> records;
        for (uint64_t edge = 1; edge <= 64; ++edge) records.push_back({IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(edge - 1)), edge), edge});
        std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
        const OrderedBTreeEntrySource source = [&]() -> OrderedBTreeEntryCursor {
            return [&, next = size_t(0)]() mutable -> std::optional<BTreePageEntry> { if (next == records.size()) return {}; return records[next++]; };
        };
        const BulkBTreeBuildOptions build{PhysicalSlotId(40), PhysicalSlotId(180), definition.index_id, records.size(), 32, BTreeLeafValueLayout::BLVL_U64};
        tx.stageScalarIndexCreate(trees, BulkBTreeBuilder::planNativeTree(trees, *metadata, tx.baseSnapshot(), definition.index_id, source, build));
        ASSERT_TRUE(tx.commit().committed()); io->flushStream();
    }
    void soloIndex() {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        (void)backend.createLogicalNodeIndex({"solo_age", "Solo", {"age"}}); backend.close();
    }
    void corruptDerivedIndex(bool ghost, bool constraint = false) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); EntityPageStore entities(io, *metadata);
        SlottedPagePool pages(io, *metadata); BTreePageStore trees(pages, *metadata);
        EntityLabelSetStore labels(io, *metadata, pages); PropertyCollectionRootStore roots(pages, *metadata);
        SystemCatalog catalog(trees, *metadata); auto pin = metadata->openSnapshot();
        const auto index = constraint ? supportId(catalog, pin) : indexId(catalog, pin, "person_age"); pin.release();
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(180);
        options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(180), 902};
        NativeGraphWritePlanner planner(*metadata, entities, labels, roots, trees, options);
        auto tx = planner.beginTransaction();
        const auto key = constraint ? supportKey(PropertyValue(int64_t(ghost ? 100 : 0)), 1) :
            IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(ghost ? 100 : 0)), 1);
        if (ghost) tx.stageScalarIndexUpsert(trees, index, key, 1); else tx.stageScalarIndexErase(trees, index, key);
        ASSERT_TRUE(tx.commit().committed()); io->flushStream();
    }
    void changeStatistics(bool unknown, bool labelUnderflow = false) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); EntityPageStore entities(io, *metadata);
        SlottedPagePool pages(io, *metadata); BTreePageStore trees(pages, *metadata);
        EntityLabelSetStore labels(io, *metadata, pages); PropertyCollectionRootStore roots(pages, *metadata);
        SystemCatalog catalog(trees, *metadata); auto pin = metadata->openSnapshot();
        const auto label = catalog.findSchemaToken(pin, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"); ASSERT_TRUE(label); pin.release();
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(180);
        options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(180), 902};
        NativeGraphWritePlanner planner(*metadata, entities, labels, roots, trees, options); auto tx = planner.beginTransaction();
        if (unknown) {
            tx.stageScalarIndexErase(trees, RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogGraphCardinalityCodec::nodeCountKey());
            tx.stageScalarIndexErase(trees, RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogGraphCardinalityCodec::edgeCountKey());
            for (const auto kind : {SystemCatalogSchemaTokenKind::SCSTK_LABEL, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE})
                tx.stageScalarIndexErase(trees, RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogTokenCardinalityCodec::completeKey(kind));
        } else {
            const auto key = labelUnderflow ? SystemCatalogTokenCardinalityCodec::countKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, *label) :
                SystemCatalogGraphCardinalityCodec::nodeCountKey();
            tx.stageScalarIndexUpsert(trees, RSS_SYSTEM_CATALOG_INDEX_ID, key, SystemCatalogGraphCardinalityCodec::encodeCount(0));
        }
        ASSERT_TRUE(tx.commit().committed()); io->flushStream();
    }
    void uniqueConstraint(bool nodeKey = false, bool edge = false, bool solo = false) {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        (void)backend.createLogicalConstraint({"unique_value", edge ? LogicalGraphConstraintEntityKind::LGCEK_EDGE : LogicalGraphConstraintEntityKind::LGCEK_NODE,
            nodeKey ? LogicalGraphConstraintKind::LGCK_NODE_KEY : LogicalGraphConstraintKind::LGCK_UNIQUE,
            edge ? "KNOWS" : solo ? "Solo" : "Person", nodeKey ? std::vector<std::string>{"age", "note"} : std::vector<std::string>{edge ? "weight" : "age"},
            LogicalGraphConstraintPropertyType::LGCPT_NONE});
        backend.close();
    }
    uint64_t supportId(SystemCatalog &catalog, const MetadataSnapshotPin &pin) {
        for (const auto &definition : catalog.readConstraintDefinitions(pin, 128)) if (definition.name == "unique_value") return definition.constraint_id;
        ADD_FAILURE() << "missing support definition"; return 0;
    }
    std::vector<std::byte> supportKey(const PropertyValue &value, uint64_t owner) {
        const std::array<PropertyValue, 1> values{value};
        return ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), owner);
    }
    void withStore(const std::function<void(RelocatableMetadataStore &, PropertyCollectionRootStore &, BTreePageStore &,
        SystemCatalog &, const MetadataSnapshotPin &, const std::shared_ptr<StorageIO> &)> &action) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io);
        SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore values(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, values, *metadata); BTreePageStore trees(pages, *metadata);
        SystemCatalog catalog(trees, *metadata); auto pin = metadata->openSnapshot();
        action(*metadata, roots, trees, catalog, pin, io); io->flushStream();
    }
    Superblock writeCell(RelocatableMetadataStore &metadata, EntityPropertyOwnerKind kind, uint64_t owner,
        uint32_t token, const PropertyValue &value, bool erase = false, MetadataPublishOptions options = {},
        const CancellationCheckpoint *cancellation = nullptr) {
        const auto before = metadata.openSnapshot().superblock();
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{kind, owner}, token, OwnedPropertyValue(value), erase}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(180), 903, cancellation, true, {}};
        try {
            auto result = metadata.mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1), options);
            EXPECT_EQ(memory->used(), 0U); return result;
        } catch (...) { EXPECT_EQ(memory->used(), 0U); throw; }
    }
    Superblock deleteOwner(RelocatableMetadataStore &metadata, EntityPropertyOwnerKind kind, uint64_t id,
        const MetadataPublishOptions &options = {}, const CancellationCheckpoint *cancellation = nullptr) {
        const auto before = metadata.openSnapshot().superblock();
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        const std::array<EntityPropertyKey, 1> owners{{{kind, id}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(180), 903, cancellation, true, owners};
        try {
            auto result = metadata.mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1), options);
            EXPECT_EQ(memory->used(), 0U); return result;
        } catch (...) { EXPECT_EQ(memory->used(), 0U); throw; }
    }
    uint64_t indexId(SystemCatalog &catalog, const MetadataSnapshotPin &pin, std::string_view name) {
        for (const auto &definition : catalog.readIndexDefinitions(pin, 128)) if (definition.name == name) return definition.index_id;
        ADD_FAILURE() << "missing index " << name; return 0;
    }
};
}
TEST_F(HybridPropertyIndexedLifecycleTest, CompositeAndScalarKeysFollowFieldRemovalAndReinsertion) {
    seed(); compositeIndex(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &) {
        const auto age = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(age);
        const auto single = indexId(catalog, old, "person_age"), composite = indexId(catalog, old, "person_age_note");
        const std::array beforeTuple{PropertyValue(int64_t(0)), PropertyValue("person")};
        const auto beforeKey = IndexKeyCodec::encodeCompositeEntryKey(beforeTuple, 1);
        ASSERT_TRUE(BTreeIndex(composite, trees, metadata).find(old, beforeKey));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *age, PropertyValue(), true);
        auto missing = metadata.openSnapshot();
        EXPECT_FALSE(roots.readEntityPropertyOptional(missing, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *age));
        EXPECT_FALSE(BTreeIndex(single, trees, metadata).find(missing, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(0)), 1)));
        EXPECT_FALSE(BTreeIndex(composite, trees, metadata).find(missing, beforeKey));
        EXPECT_TRUE(BTreeIndex(composite, trees, metadata).find(old, beforeKey));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *age, PropertyValue(int64_t(100)));
        auto current = metadata.openSnapshot(); const std::array afterTuple{PropertyValue(int64_t(100)), PropertyValue("person")};
        EXPECT_EQ(BTreeIndex(single, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(100)), 1)), std::optional<uint64_t>(1));
        EXPECT_EQ(BTreeIndex(composite, trees, metadata).find(current, IndexKeyCodec::encodeCompositeEntryKey(afterTuple, 1)), std::optional<uint64_t>(1));
    });
    EXPECT_EQ(read()[1], (Rows{{PropertyValue(int64_t(64)), PropertyValue(int64_t(2116))}}));
}
TEST_F(HybridPropertyIndexedLifecycleTest, UnindexedFieldChangeDoesNotCopyAnUnchangedScalarIndexRoot) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &trees, auto &catalog, const auto &old, auto &) {
        const auto note = catalog.findPropertyToken(old, "note"); ASSERT_TRUE(note);
        const auto index = indexId(catalog, old, "person_age"); const auto root = metadata.findIndexRoot(old, index);
        const auto location = metadata.findPageLocation(old, root.root_page_id);
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 8, *note, PropertyValue("changed"));
        auto current = metadata.openSnapshot(); EXPECT_EQ(metadata.findIndexRoot(current, index), root);
        EXPECT_EQ(metadata.findPageLocation(current, root.root_page_id), location);
        EXPECT_TRUE(BTreeIndex(index, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(7)), 8)));
    });
    EXPECT_EQ(read()[2], (Rows{{PropertyValue("changed"), PropertyValue()}}));
}
TEST_F(HybridPropertyIndexedLifecycleTest, ScalarIndexTracksCanonicalNumericTypeChange) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &) {
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto index = indexId(catalog, old, "person_age");
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(100.0));
        auto current = metadata.openSnapshot();
        const auto value = roots.readEntityPropertyOptional(current, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *token);
        ASSERT_TRUE(value); EXPECT_EQ(value->value, PropertyValue(100.0));
        EXPECT_FALSE(BTreeIndex(index, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(0)), 1)));
        EXPECT_EQ(BTreeIndex(index, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(100.0), 1)), std::optional<uint64_t>(1));
        EXPECT_TRUE(BTreeIndex(index, trees, metadata).find(old, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(0)), 1)));
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, BootstrapFaultKeepsScalarIndexAndFieldInTheSameRecoveredVersion) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE); setAge(100);
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        withStore([&](auto &metadata, auto &, auto &, auto &catalog, const auto &pin, auto &) {
            const auto token = catalog.findPropertyToken(pin, "age"); ASSERT_TRUE(token);
            MetadataPublishOptions options; options.bootstrap_failpoint = point;
            EXPECT_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(101)), false, options), BootstrapPublishInterrupted);
        });
        withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &pin, auto &) {
            const auto token = catalog.findPropertyToken(pin, "age"); ASSERT_TRUE(token);
            const int64_t expected = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE ? 101 : 100;
            const int64_t absent = expected == 101 ? 100 : 101;
            const auto value = roots.readEntityPropertyOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *token);
            ASSERT_TRUE(value); EXPECT_EQ(value->value, PropertyValue(expected));
            const auto index = indexId(catalog, pin, "person_age");
            EXPECT_EQ(BTreeIndex(index, trees, metadata).find(pin, IndexKeyCodec::encodeEntryKey(PropertyValue(expected), 1)), std::optional<uint64_t>(1));
            EXPECT_FALSE(BTreeIndex(index, trees, metadata).find(pin, IndexKeyCodec::encodeEntryKey(PropertyValue(absent), 1)));
        });
    }
}

TEST_F(HybridPropertyIndexedLifecycleTest, EmptyPartialIndexKeepsItsRootAndAcceptsReinsertion) {
    seed(false, false, true); soloIndex(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &trees, auto &catalog, const auto &old, auto &) {
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto index = indexId(catalog, old, "solo_age");
        const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(0)), 1);
        ASSERT_EQ(BTreeIndex(index, trees, metadata).find(old, key), std::optional<uint64_t>(1));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(), true);
        auto empty = metadata.openSnapshot(); ASSERT_TRUE(metadata.findIndexRootOptional(empty, index));
        EXPECT_FALSE(BTreeIndex(index, trees, metadata).find(empty, key));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)));
        auto current = metadata.openSnapshot();
        EXPECT_EQ(BTreeIndex(index, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(100)), 1)), std::optional<uint64_t>(1));
        EXPECT_TRUE(BTreeIndex(index, trees, metadata).find(old, key));
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, UnmatchedPartialIndexDoesNotCopyItsRootWhenItsPropertyChanges) {
    seed(true, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &, auto &catalog, const auto &old, auto &) {
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto index = indexId(catalog, old, "person_age"); const auto root = metadata.findIndexRoot(old, index);
        const auto location = metadata.findPageLocation(old, root.root_page_id);
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)));
        auto current = metadata.openSnapshot(); EXPECT_EQ(metadata.findIndexRoot(current, index), root);
        EXPECT_EQ(metadata.findPageLocation(current, root.root_page_id), location);
    });
    EXPECT_EQ(read()[1], (Rows{{PropertyValue(int64_t(63)), PropertyValue(int64_t(2016))}}));
}
TEST_F(HybridPropertyIndexedLifecycleTest, MissingOldIndexKeyRefusesFieldPublicationInsteadOfRepairingItSilently) {
    seed(); corruptDerivedIndex(false); convert(EntityPropertyOwnerKind::EPOK_NODE); setAge(100, true);
}
TEST_F(HybridPropertyIndexedLifecycleTest, ExistingNewGhostKeyRefusesFieldPublication) {
    seed(); corruptDerivedIndex(true); convert(EntityPropertyOwnerKind::EPOK_NODE); setAge(100, true);
}
TEST_F(HybridPropertyIndexedLifecycleTest, CancellationAfterIndexImagePreparationPublishesNeitherRootAndAllowsRetry) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &pin, auto &io) {
        const auto token = catalog.findPropertyToken(pin, "age"); ASSERT_TRUE(token);
        (void)metadata.reserveNativeGraphLogicalIds(1, 0); // complete writable-open recovery
        const auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        struct Cancel final : CancellationCheckpoint { bool armed = false; void check() const override { if (armed) throw std::runtime_error("indexed mutation cancelled"); } } cancellation;
        {
            debug::PerfTrace::ScopedObserver observer(&cancellation, [](void *context, std::string_view key, int64_t value, bool) noexcept {
                if (key == "native_graph.hybrid.private_scalar_index_images" && value > 0) static_cast<Cancel *>(context)->armed = true;
            });
            try { (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)), false, {}, &cancellation); FAIL(); }
            catch (const std::runtime_error &error) { EXPECT_EQ(std::string(error.what()), "indexed mutation cancelled"); }
            EXPECT_TRUE(cancellation.armed);
        }
        EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        auto unchanged = metadata.openSnapshot(); const auto value = roots.readEntityPropertyOptional(unchanged, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *token);
        ASSERT_TRUE(value); EXPECT_EQ(value->value, PropertyValue(int64_t(0)));
        const auto index = indexId(catalog, unchanged, "person_age");
        EXPECT_TRUE(BTreeIndex(index, trees, metadata).find(unchanged, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(0)), 1)));
        EXPECT_FALSE(BTreeIndex(index, trees, metadata).find(unchanged, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(100)), 1)));
        EXPECT_NO_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100))));
    });
}

TEST_F(HybridPropertyIndexedLifecycleTest, MultipleOwnersShareOneScalarLeafPreparationAndPreserveAllOldKeys) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &) {
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto before = metadata.openSnapshot().superblock();
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
            {{EntityPropertyOwnerKind::EPOK_NODE, 1}, *token, int64_t(100)},
            {{EntityPropertyOwnerKind::EPOK_NODE, 2}, *token, int64_t(101)}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(180), 903, nullptr, true, {}};
        int64_t frames = 0;
        {
            debug::PerfTrace::ScopedObserver observe(&frames, [](void *context, std::string_view key, int64_t value, bool) noexcept {
                if (key == "native_graph.hybrid.pool_frames_prepared") *static_cast<int64_t *>(context) += value;
            });
            EXPECT_NO_THROW((void)metadata.mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
        }
        EXPECT_GT(frames, 0); EXPECT_LE(frames, 3); EXPECT_EQ(memory->used(), 0U);
        const auto index = indexId(catalog, old, "person_age"); auto current = metadata.openSnapshot();
        for (uint64_t owner = 1; owner <= 2; ++owner) {
            const auto value = roots.readEntityPropertyOptional(current, {EntityPropertyOwnerKind::EPOK_NODE, owner}, *token);
            ASSERT_TRUE(value); EXPECT_EQ(value->value, PropertyValue(int64_t(owner + 99)));
            const auto oldKey = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(owner - 1)), owner);
            EXPECT_FALSE(BTreeIndex(index, trees, metadata).find(current, oldKey)); EXPECT_TRUE(BTreeIndex(index, trees, metadata).find(old, oldKey));
            EXPECT_TRUE(BTreeIndex(index, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(owner + 99)), owner)));
        }

    });
    EXPECT_EQ(read()[1], (Rows{{PropertyValue(int64_t(64)), PropertyValue(int64_t(2216))}}));
}
TEST_F(HybridPropertyIndexedLifecycleTest, EdgePropertyIndexAndCanonicalTopologyRemainConsistentAfterFieldUpdate) {
    seed(); edgeIndex(); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &io) {
        const auto token = catalog.findPropertyToken(old, "weight"); ASSERT_TRUE(token);
        const auto index = indexId(catalog, old, "edge_weight");
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_EDGE, 1, *token, PropertyValue(int64_t(100)));
        auto current = metadata.openSnapshot();
        EXPECT_EQ(current.superblock().edge_entity_directory_root, old.superblock().edge_entity_directory_root);
        EXPECT_EQ(current.superblock().node_entity_directory_root, old.superblock().node_entity_directory_root);
        const auto value = roots.readEntityPropertyOptional(current, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, *token);
        ASSERT_TRUE(value); EXPECT_EQ(value->value, PropertyValue(int64_t(100)));
        const auto oldKey = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(0)), 1);
        EXPECT_FALSE(BTreeIndex(index, trees, metadata).find(current, oldKey)); EXPECT_TRUE(BTreeIndex(index, trees, metadata).find(old, oldKey));
        EXPECT_EQ(BTreeIndex(index, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(100)), 1)), std::optional<uint64_t>(1));
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io, metadata, memory).scan());
    });
    const auto rows = read()[4]; ASSERT_EQ(rows.size(), 8U);
    EXPECT_EQ(rows[0][1], PropertyValue(int64_t(1))); EXPECT_EQ(rows[1][1], PropertyValue(int64_t(100)));
}

TEST_F(HybridPropertyIndexedLifecycleTest, OversizedIndexedValueIsStoredAndOnlyItsAccelerationEntryIsOmitted) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &io) {
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto index = indexId(catalog, old, "person_age"); const PropertyValue large(std::string(8192, 'x'));
        ASSERT_FALSE(IndexKeyCodec::tryEncodeEntryKey(large, 1));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, large);
        auto big = metadata.openSnapshot(); const auto actual = roots.readEntityPropertyOptional(big, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *token);
        ASSERT_TRUE(actual); EXPECT_EQ(actual->value, large);
        const auto oldKey = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(0)), 1);
        EXPECT_FALSE(BTreeIndex(index, trees, metadata).find(big, oldKey)); EXPECT_TRUE(BTreeIndex(index, trees, metadata).find(old, oldKey));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)));
        auto current = metadata.openSnapshot();
        EXPECT_EQ(BTreeIndex(index, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(100)), 1)), std::optional<uint64_t>(1));
        EXPECT_EQ(roots.readEntityPropertyOptional(big, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *token)->value, large);
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io, metadata, memory).scan());
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, NonNullViolationRefusesBeforePrivateIoAndKeepsIndexAndOldPin) {
    seed(true, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &io) {
        (void)metadata.reserveNativeGraphLogicalIds(1, 0);
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        for (bool erase : {false, true}) {
            EXPECT_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(), erase), std::runtime_error);
            EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        }
        const auto index = indexId(catalog, old, "person_age");
        EXPECT_EQ(BTreeIndex(index, trees, metadata).find(old, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(0)), 1)), std::optional<uint64_t>(1));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)));
        auto current = metadata.openSnapshot();
        EXPECT_EQ(BTreeIndex(index, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(100)), 1)), std::optional<uint64_t>(1));
        EXPECT_EQ(roots.readEntityPropertyOptional(old, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *token)->value, PropertyValue(int64_t(0)));
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, IntegerConstraintRejectsDoubleButAllowsValidChangeAndMissingValue) {
    seed();
    {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        (void)backend.createLogicalConstraint({"person_age_integer", LogicalGraphConstraintEntityKind::LGCEK_NODE,
            LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE, "Person", {"age"}, LogicalGraphConstraintPropertyType::LGCPT_INTEGER});
        backend.close();
    }
    convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &trees, auto &catalog, const auto &old, auto &io) {
        (void)metadata.reserveNativeGraphLogicalIds(1, 0);
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        EXPECT_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(100.0)), std::runtime_error);
        EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(), true);
        const auto current = metadata.openSnapshot(); const auto index = indexId(catalog, old, "person_age");
        EXPECT_FALSE(BTreeIndex(index, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(100)), 1)));
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, RelationshipTypeConstraintChecksFinalNativeEdgeValue) {
    seed(false);
    {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        (void)backend.createLogicalConstraint({"knows_weight_integer", LogicalGraphConstraintEntityKind::LGCEK_EDGE,
            LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE, "KNOWS", {"weight"}, LogicalGraphConstraintPropertyType::LGCPT_INTEGER});
        backend.close();
    }
    edgeIndex(); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    withStore([&](auto &metadata, auto &roots, auto &, auto &catalog, const auto &old, auto &io) {
        (void)metadata.reserveNativeGraphLogicalIds(1, 0);
        const auto token = catalog.findPropertyToken(old, "weight"); ASSERT_TRUE(token);
        const auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        EXPECT_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_EDGE, 1, *token, PropertyValue("bad")), std::runtime_error);
        EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_EDGE, 1, *token, PropertyValue(int64_t(100)));
        EXPECT_EQ(roots.readEntityPropertyOptional(metadata.openSnapshot(), {EntityPropertyOwnerKind::EPOK_EDGE, 1}, *token)->value, PropertyValue(int64_t(100)));
        EXPECT_EQ(roots.readEntityPropertyOptional(old, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, *token)->value, PropertyValue(int64_t(0)));
    });
}

TEST_F(HybridPropertyIndexedLifecycleTest, UniqueSupportAndScalarIndexFollowValidWritesAndRefuseDuplicates) {
    seed(); uniqueConstraint(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &io) {
        (void)metadata.reserveNativeGraphLogicalIds(1, 0);
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto support = supportId(catalog, old); const auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        EXPECT_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(1))), std::runtime_error);
        EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)));
        auto current = metadata.openSnapshot(); BTreeIndex tree(support, trees, metadata);
        EXPECT_EQ(tree.find(current, supportKey(PropertyValue(int64_t(100)), 1)), std::optional<uint64_t>(1));
        EXPECT_FALSE(tree.find(current, supportKey(PropertyValue(int64_t(0)), 1)));
        EXPECT_EQ(tree.find(old, supportKey(PropertyValue(int64_t(0)), 1)), std::optional<uint64_t>(1));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(), true);
        EXPECT_FALSE(tree.find(metadata.openSnapshot(), supportKey(PropertyValue(int64_t(100)), 1)));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(101)));
        EXPECT_EQ(roots.readEntityPropertyOptional(old, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *token)->value, PropertyValue(int64_t(0)));
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io, metadata, memory).scan());
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, CompositeNodeKeyAllowsAtomicSwapAndRefusesMissingOrBatchDuplicate) {
    seed(); uniqueConstraint(true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &io) {
        (void)metadata.reserveNativeGraphLogicalIds(1, 0);
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        EXPECT_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(), true), std::runtime_error);
        EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        for (const bool duplicate : {true, false}) {
            before = metadata.openSnapshot().superblock();
            auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
            const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
                {{EntityPropertyOwnerKind::EPOK_NODE, 1}, *token, int64_t(duplicate ? 100 : 1)},
                {{EntityPropertyOwnerKind::EPOK_NODE, 2}, *token, int64_t(duplicate ? 100 : 0)}}};
            NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
                cells, memory, PhysicalSlotId(40), PhysicalSlotId(180), 903, nullptr, true, {}};
            if (duplicate) {
                EXPECT_THROW((void)metadata.mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), std::runtime_error);
                EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
            } else EXPECT_NO_THROW((void)metadata.mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
            EXPECT_EQ(memory->used(), 0U);
        }
        const auto current = metadata.openSnapshot(); const auto support = supportId(catalog, old);
        for (uint64_t owner : {1U, 2U}) {
            const int64_t value = owner == 1 ? 1 : 0;
            const std::array<PropertyValue, 2> values{PropertyValue(value), PropertyValue("person")};
            EXPECT_EQ(BTreeIndex(support, trees, metadata).find(current,
                ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), owner)), std::optional(owner));
            EXPECT_EQ(roots.readEntityPropertyOptional(current, {EntityPropertyOwnerKind::EPOK_NODE, owner}, *token)->value, PropertyValue(value));
        }
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, EdgeUniqueSupportSurvivesBothBootstrapInterruptionBoundaries) {
    seed(false); uniqueConstraint(false, true); edgeIndex(); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    withStore([&](auto &metadata, auto &, auto &, auto &catalog, const auto &pin, auto &) {
        const auto token = catalog.findPropertyToken(pin, "weight"); ASSERT_TRUE(token);
        EXPECT_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_EDGE, 1, *token, PropertyValue(int64_t(1))), std::runtime_error);
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_EDGE, 1, *token, PropertyValue(int64_t(100)));
    });
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        withStore([&](auto &metadata, auto &, auto &, auto &catalog, const auto &pin, auto &) {
            const auto token = catalog.findPropertyToken(pin, "weight"); ASSERT_TRUE(token);
            MetadataPublishOptions options; options.bootstrap_failpoint = point;
            EXPECT_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_EDGE, 1, *token, PropertyValue(int64_t(101)), false, options), BootstrapPublishInterrupted);
        });
        withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &pin, auto &io) {
            const auto expected = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE ? int64_t(101) : int64_t(100);
            const auto token = catalog.findPropertyToken(pin, "weight"); ASSERT_TRUE(token);
            EXPECT_EQ(roots.readEntityPropertyOptional(pin, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, *token)->value, PropertyValue(expected));
            const auto support = supportId(catalog, pin);
            EXPECT_EQ(BTreeIndex(support, trees, metadata).find(pin, supportKey(PropertyValue(expected), 1)), std::optional<uint64_t>(1));
            EXPECT_FALSE(BTreeIndex(support, trees, metadata).find(pin, supportKey(PropertyValue(expected == 100 ? int64_t(101) : int64_t(100)), 1)));
            auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io, metadata, memory).scan());
        });
    }
}
TEST_F(HybridPropertyIndexedLifecycleTest, UniqueConstraintStillChecksValuesBeyondScalarIndexKeyLimit) {
    seed(); uniqueConstraint(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &trees, auto &catalog, const auto &old, auto &io) {
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const PropertyValue large(std::string(8192, 'u'));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, large);
        const auto support = supportId(catalog, old); const auto pin = metadata.openSnapshot();
        EXPECT_EQ(BTreeIndex(support, trees, metadata).find(pin, supportKey(large, 1)), std::optional<uint64_t>(1));
        const auto before = pin.superblock(); const auto epoch = io->dataMutationEpoch();
        EXPECT_THROW((void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 2, *token, large), std::runtime_error);
        EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
    });
}

TEST_F(HybridPropertyIndexedLifecycleTest, MissingConstraintSupportEntryRefusesWithoutRepairOrPrivateIo) {
    seed(false); uniqueConstraint(); corruptDerivedIndex(false, true);
    convert(EntityPropertyOwnerKind::EPOK_NODE); setAge(100, true);
}
TEST_F(HybridPropertyIndexedLifecycleTest, GhostConstraintSupportEntryRefusesWithoutRepairOrPrivateIo) {
    seed(false); uniqueConstraint(); corruptDerivedIndex(true, true);
    convert(EntityPropertyOwnerKind::EPOK_NODE); setAge(100, true);
}

TEST_F(HybridPropertyIndexedLifecycleTest, LastUniqueSupportKeyKeepsActiveEmptyRootAndCanBeReinserted) {
    seed(false, false, true); uniqueConstraint(false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &trees, auto &catalog, const auto &old, auto &) {
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto support = supportId(catalog, old);
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(), true);
        const auto empty = metadata.openSnapshot(); EXPECT_TRUE(metadata.findIndexRootOptional(empty, support));
        EXPECT_TRUE(BTreeIndex(support, trees, metadata).scan(empty, {}, 2).empty());
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)));
        EXPECT_EQ(BTreeIndex(support, trees, metadata).find(metadata.openSnapshot(), supportKey(PropertyValue(int64_t(100)), 1)), std::optional<uint64_t>(1));
        EXPECT_EQ(BTreeIndex(support, trees, metadata).find(old, supportKey(PropertyValue(int64_t(0)), 1)), std::optional<uint64_t>(1));
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, UnchangedUniqueKeyAndNonmatchingOwnerKeepSupportPhysicalRoot) {
    seed(false, false, true); uniqueConstraint(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &, auto &catalog, const auto &old, auto &) {
        const auto age = catalog.findPropertyToken(old, "age"), note = catalog.findPropertyToken(old, "note"); ASSERT_TRUE(age); ASSERT_TRUE(note);
        const auto support = supportId(catalog, old); const auto root = metadata.findIndexRoot(old, support);
        const auto location = metadata.findPageLocation(old, root.root_page_id);
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 2, *note, PropertyValue("new"));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *age, PropertyValue(), true);
        const auto current = metadata.openSnapshot(); EXPECT_EQ(metadata.findIndexRoot(current, support), root);
        EXPECT_EQ(metadata.findPageLocation(current, root.root_page_id), location);
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, CancellationAfterConstraintImagesKeepsBothVersionsAndAllowsRetry) {
    seed(); uniqueConstraint(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &trees, auto &catalog, const auto &old, auto &io) {
        (void)metadata.reserveNativeGraphLogicalIds(1, 0);
        const auto token = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(token);
        const auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        struct Cancel final : CancellationCheckpoint { bool armed = false; void check() const override { if (armed) throw std::runtime_error("constraint cancelled"); } } cancellation;
        {
            debug::PerfTrace::ScopedObserver observer(&cancellation, [](void *context, std::string_view key, int64_t value, bool) noexcept {
                if (key == "native_graph.hybrid.private_constraint_index_images" && value > 0) static_cast<Cancel *>(context)->armed = true;
            });
            try { (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)), false, {}, &cancellation); FAIL(); }
            catch (const std::runtime_error &error) { EXPECT_EQ(std::string(error.what()), "constraint cancelled"); }
            EXPECT_TRUE(cancellation.armed);
        }
        EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        const auto support = supportId(catalog, old);
        EXPECT_EQ(BTreeIndex(support, trees, metadata).find(old, supportKey(PropertyValue(int64_t(0)), 1)), std::optional<uint64_t>(1));
        (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *token, PropertyValue(int64_t(100)));
        EXPECT_EQ(BTreeIndex(support, trees, metadata).find(metadata.openSnapshot(), supportKey(PropertyValue(int64_t(100)), 1)), std::optional<uint64_t>(1));
    });
}

TEST_F(HybridPropertyIndexedLifecycleTest, IsolatedNodeDeletionRemovesAllDerivedKeysAndUpdatesKnownCounts) {
    seed(); uniqueConstraint(); compositeIndex(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &io) {
        const auto age = catalog.findPropertyToken(old, "age"); ASSERT_TRUE(age);
        const auto support = supportId(catalog, old);
        const auto label = catalog.findSchemaToken(old, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"); ASSERT_TRUE(label);
        const auto labelCount = catalog.readTokenCardinality(old, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *label);
        ASSERT_TRUE(labelCount); EXPECT_EQ(*labelCount, 64U);
        (void)deleteOwner(metadata, EntityPropertyOwnerKind::EPOK_NODE, 64);
        const auto current = metadata.openSnapshot(); EXPECT_FALSE(metadata.findNodePageLocationOptional(current, 64));
        EXPECT_TRUE(metadata.findNodePageLocationOptional(old, 64));
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(current), (std::optional<NativeGraphCardinalityStatistics>{{63, 64}}));
        EXPECT_EQ(catalog.readTokenCardinality(current, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *label), std::optional<uint64_t>(63));
        EXPECT_EQ(catalog.readTokenCardinality(old, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *label), std::optional<uint64_t>(64));
        EXPECT_FALSE(BTreeIndex(support, trees, metadata).find(current, supportKey(PropertyValue(int64_t(63)), 64)));
        EXPECT_TRUE(BTreeIndex(support, trees, metadata).find(old, supportKey(PropertyValue(int64_t(63)), 64)));
        const auto scalar = indexId(catalog, old, "person_age");
        EXPECT_FALSE(BTreeIndex(scalar, trees, metadata).find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(63)), 64)));
        EXPECT_EQ(roots.readEntityPropertyOptional(old, {EntityPropertyOwnerKind::EPOK_NODE, 64}, *age)->value, PropertyValue(int64_t(63)));
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io, metadata, memory).scan());
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, IncidentNodeRefusalKeepsCatalogIndexesAndPhysicalEpoch) {
    seed(); uniqueConstraint(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &, auto &, const auto &, auto &io) {
        (void)metadata.reserveNativeGraphLogicalIds(1, 0);
        const auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        EXPECT_THROW((void)deleteOwner(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1), FormatException);
        EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, EdgeDeletionPublishesTopologySupportAndCountsAcrossBootstrapFaults) {
    seed(false); uniqueConstraint(false, true); edgeIndex(); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        withStore([&](auto &metadata, auto &, auto &, auto &, const auto &, auto &) {
            MetadataPublishOptions options; options.bootstrap_failpoint = point;
            EXPECT_THROW((void)deleteOwner(metadata, EntityPropertyOwnerKind::EPOK_EDGE, 64, options), BootstrapPublishInterrupted);
        });
        withStore([&](auto &metadata, auto &, auto &trees, auto &catalog, const auto &pin, auto &io) {
            const bool committed = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
            EXPECT_EQ(bool(metadata.findEdgePageLocationOptional(pin, 64)), !committed);
            const auto counts = catalog.readGraphCardinalityStatistics(pin); ASSERT_TRUE(counts);
            EXPECT_EQ(counts->node_count, 64U); EXPECT_EQ(counts->edge_count, committed ? 63U : 64U);
            const auto type = catalog.findSchemaToken(pin, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "KNOWS"); ASSERT_TRUE(type);
            EXPECT_EQ(catalog.readTokenCardinality(pin, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, *type), std::optional<uint64_t>(committed ? 63 : 64));
            const auto support = supportId(catalog, pin);
            EXPECT_EQ(bool(BTreeIndex(support, trees, metadata).find(pin, supportKey(PropertyValue(int64_t(63)), 64))), !committed);
            auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io, metadata, memory).scan());
        });
    }
}

TEST_F(HybridPropertyIndexedLifecycleTest, OwnerDeletionPreservesUnknownStatisticsAndDoesNotRewriteCatalog) {
    seed(); uniqueConstraint(); changeStatistics(true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &trees, auto &catalog, const auto &old, auto &) {
        EXPECT_FALSE(catalog.readGraphCardinalityStatistics(old));
        const auto label = catalog.findSchemaToken(old, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"); ASSERT_TRUE(label);
        EXPECT_FALSE(catalog.readTokenCardinality(old, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *label));
        const auto root = metadata.findIndexRoot(old, RSS_SYSTEM_CATALOG_INDEX_ID); const auto location = metadata.findPageLocation(old, root.root_page_id);
        const auto counterKey = SystemCatalogTokenCardinalityCodec::countKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, *label);
        const auto incomplete = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, trees, metadata).find(old, counterKey);
        (void)deleteOwner(metadata, EntityPropertyOwnerKind::EPOK_NODE, 64);
        const auto current = metadata.openSnapshot(); EXPECT_FALSE(metadata.findNodePageLocationOptional(current, 64));
        EXPECT_FALSE(catalog.readGraphCardinalityStatistics(current));
        EXPECT_FALSE(catalog.readTokenCardinality(current, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *label));
        EXPECT_EQ(metadata.findIndexRoot(current, RSS_SYSTEM_CATALOG_INDEX_ID), root);
        EXPECT_EQ(metadata.findPageLocation(current, root.root_page_id), location);
        EXPECT_EQ(BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, trees, metadata).find(current, counterKey), incomplete);
    });
}
TEST_F(HybridPropertyIndexedLifecycleTest, OwnerDeletionRefusesGraphCountUnderflowBeforePhysicalIo) {
    seed(); changeStatistics(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    setAge(100, true, true);
}
TEST_F(HybridPropertyIndexedLifecycleTest, OwnerDeletionRefusesCompleteLabelCountUnderflowBeforePhysicalIo) {
    seed(); changeStatistics(false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    setAge(100, true, true);
}
TEST_F(HybridPropertyIndexedLifecycleTest, CancellationAfterCatalogImagesKeepsEntityIndexesCountsAndOldPin) {
    seed(); uniqueConstraint(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &, auto &catalog, const auto &old, auto &io) {
        (void)metadata.reserveNativeGraphLogicalIds(1, 0);
        const auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        struct Cancel final : CancellationCheckpoint { bool armed = false; void check() const override { if (armed) throw std::runtime_error("catalog cancelled"); } } cancellation;
        {
            debug::PerfTrace::ScopedObserver observer(&cancellation, [](void *context, std::string_view key, int64_t value, bool) noexcept {
                if (key == "native_graph.hybrid.private_catalog_index_images" && value > 0) static_cast<Cancel *>(context)->armed = true;
            });
            try { (void)deleteOwner(metadata, EntityPropertyOwnerKind::EPOK_NODE, 64, {}, &cancellation); FAIL(); }
            catch (const std::runtime_error &error) { EXPECT_EQ(std::string(error.what()), "catalog cancelled"); }
            EXPECT_TRUE(cancellation.armed);
        }
        EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        (void)deleteOwner(metadata, EntityPropertyOwnerKind::EPOK_NODE, 64);
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(metadata.openSnapshot()), (std::optional<NativeGraphCardinalityStatistics>{{63, 64}}));
        EXPECT_TRUE(metadata.findNodePageLocationOptional(old, 64)); EXPECT_FALSE(metadata.findNodePageLocationOptional(metadata.openSnapshot(), 64));
    });
}

TEST_F(HybridPropertyIndexedLifecycleTest, CatalogDeletionQuotaFailureRefundsAndPreservesAllPublishedRoots) {
    seed(); uniqueConstraint(); compositeIndex(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &, auto &, auto &, const auto &, auto &io) {
        (void)metadata.reserveNativeGraphLogicalIds(1, 0);
        const auto before = metadata.openSnapshot().superblock(); const auto epoch = io->dataMutationEpoch();
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(64 * 1024);
        const std::array<EntityPropertyKey, 1> owners{{{EntityPropertyOwnerKind::EPOK_NODE, 64}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(180), 903, nullptr, true, owners};
        EXPECT_THROW((void)metadata.mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata.openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        EXPECT_NO_THROW((void)deleteOwner(metadata, EntityPropertyOwnerKind::EPOK_NODE, 64));
    });
}

TEST_F(HybridPropertyIndexedLifecycleTest, SameExecutorPreservesUnsortedDuplicateNeighborGathersAcrossBothLayouts) {
    seed(true, false, false, true); const auto baseline = read();
    ASSERT_EQ(baseline[6].size(), 64U);
    for (size_t i = 0; i < 64; ++i) {
        const auto target = int64_t(((i % 32U) * 13U) % 32U);
        EXPECT_EQ(baseline[6][i], (std::vector<PropertyValue>{PropertyValue(int64_t(i)), PropertyValue(target),
            PropertyValue(target == 7 ? "seven" : "person"), PropertyValue()}));
    }
    convert(EntityPropertyOwnerKind::EPOK_NODE); EXPECT_EQ(read(), baseline);
    convert(EntityPropertyOwnerKind::EPOK_EDGE); EXPECT_EQ(read(), baseline);
    convert(EntityPropertyOwnerKind::EPOK_NODE, true); EXPECT_EQ(read(), baseline);
    convert(EntityPropertyOwnerKind::EPOK_EDGE, true); EXPECT_EQ(read(), baseline);
}

TEST_F(HybridPropertyIndexedLifecycleTest, SustainedTypeChangesStayFieldLocalWithoutAutomaticRepromotion) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    withStore([&](auto &metadata, auto &roots, auto &trees, auto &catalog, const auto &old, auto &io) {
        const auto age = catalog.findPropertyToken(old, "age"), note = catalog.findPropertyToken(old, "note"); ASSERT_TRUE(age); ASSERT_TRUE(note);
        const auto catalogRoot = metadata.findIndexRoot(old, RSS_SYSTEM_CATALOG_INDEX_ID);
        const auto catalogLocation = metadata.findPageLocation(old, catalogRoot.root_page_id);
        const std::array<PropertyValue, 6> values{PropertyValue(100.0), PropertyValue(int64_t(100)), PropertyValue(),
            PropertyValue(true), PropertyValue(std::string(8192, 'm')), PropertyValue(int64_t(101))};
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        for (size_t cycle = 0; cycle < 2; ++cycle) {
            for (const auto &value : values) {
                (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *age, value);
                const auto current = metadata.openSnapshot();
                EXPECT_EQ(roots.readEntityPropertyOptional(current, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *age)->value, value);
                EXPECT_EQ(roots.readEntityPropertyOptional(current, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *note)->value, PropertyValue("person"));
                EXPECT_EQ(roots.readEntityPropertyOptional(current, {EntityPropertyOwnerKind::EPOK_NODE, 2}, *age)->value, PropertyValue(int64_t(1)));
                SlottedPagePool pages(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
                LargeValueStore large(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
                HybridPropertyReadContext context(io, pages, metadata, current);
                const auto directory = HybridPropertyDirectoryIntegrity::verify(context, current, memory, &large);
                EXPECT_EQ(directory.columns, 0U); EXPECT_EQ(directory.memberships, 128U);
                EXPECT_EQ(directory.extensions, 128U);
                EXPECT_EQ(roots.readEntityPropertyOptional(old, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *age)->value, PropertyValue(int64_t(0)));
                EXPECT_EQ(memory->used(), 0U);
            }
            (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *age, PropertyValue(), true);
            const auto missing = metadata.openSnapshot();
            EXPECT_FALSE(roots.readEntityPropertyOptional(missing, {EntityPropertyOwnerKind::EPOK_NODE, 1}, *age));
            (void)writeCell(metadata, EntityPropertyOwnerKind::EPOK_NODE, 1, *age, PropertyValue(int64_t(102)));
        }
        const auto current = metadata.openSnapshot();
        EXPECT_EQ(metadata.findIndexRoot(current, RSS_SYSTEM_CATALOG_INDEX_ID), catalogRoot);
        EXPECT_EQ(metadata.findPageLocation(current, catalogRoot.root_page_id), catalogLocation);
        const auto costs = NativeStorageAllocationCensus(io, metadata, memory).inspect(current);
        ASSERT_TRUE(costs); EXPECT_TRUE(costs->fullyAttributed());
        EXPECT_GE(costs->totalAllocatedBytes(), costs->steadyAllocatedBytes() + costs->retired_catalog_bytes);
        EXPECT_GT(costs->retired_catalog_bytes, 0U);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io, metadata, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    });
    convert(EntityPropertyOwnerKind::EPOK_NODE, true);
    EXPECT_EQ(read()[5].front(), (std::vector<PropertyValue>{PropertyValue(int64_t(102))}));
}

TEST_F(HybridPropertyIndexedLifecycleTest, CoveredReferenceControlAndHybridPropertiesShareTheSameExecutorSnapshot) {
    seed(true, false, false, true); buildCovered(); const auto baseline = read();
    convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    EXPECT_EQ(controlRows(true, memory), baseline);
    setAge(100); EXPECT_EQ(controlRows(true, memory), controlRows(false, memory));
}

#endif
