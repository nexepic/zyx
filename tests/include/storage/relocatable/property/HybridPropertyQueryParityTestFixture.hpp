#pragma once
#include <cstdlib>

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "CypherParserImpl.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/adjacency/CoveredAdjacencyCandidate.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/LogicalGraphTransactionMemoryProvider.hpp"
#include <iostream>
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/maintenance/NativeStorageAllocationCensus.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"

using namespace graph;
using namespace graph::storage;
namespace hybrid_property_query_parity_test {
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
    void seed(bool propertyIndex = true, bool constraint = false, bool firstSolo = false, bool interleavedEdges = false, size_t edgeCount = 64, bool hotSource = false, bool mixedUntyped = false, bool vectors = false, size_t nodeCount = 64) {
        materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        std::vector<LogicalGraphNodeCreateRequest> nodes;
        for (int64_t i = 0; i < int64_t(nodeCount); ++i)
            nodes.push_back({{firstSolo && i == 0 ? "Solo" : "Person"}, {{"age", PropertyValue(i)}, {"note", PropertyValue(i == 7 ? "seven" : "person")}}});
        if (vectors) for (size_t i = 0; i < nodes.size(); ++i)
            nodes[i].properties.emplace_back("embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(i)), PropertyValue(int64_t(1))}));
        const auto ids = backend.createLogicalNodes(nodes); ASSERT_EQ(ids.size(), nodeCount);
        std::vector<LogicalGraphEdgeCreateRequest> edges;
        for (size_t i = 0; i < edgeCount; ++i)
            edges.push_back({ids[hotSource ? 0U : i / 2U], ids[interleavedEdges ? ((i % 32U) * 13U) % 32U : (i / 2U + 1U) % ids.size()], mixedUntyped && i % 3U == 0U ? "" : "KNOWS", {{"weight", PropertyValue(int64_t(i))}}});
        ASSERT_EQ(backend.createLogicalEdges(edges).size(), edgeCount);
        if (propertyIndex) static_cast<void>(backend.createLogicalNodeIndex({"person_age", "Person", {"age"}}));
        if (vectors) static_cast<void>(backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2, "l2"}));
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
    void addCatalogControl(bool unknown, size_t unusedTokens = 0, uint32_t vectorDimensions = 0, bool vectorToken = true) {
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
        if (vectorDimensions) {
            IndexDefinition definition; definition.name = "vector_control"; definition.kind = IndexDefinitionKind::IDK_VECTOR;
            definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
            definition.label_token_id = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
            definition.property_names = {"embedding"}; definition.vector_dimensions = vectorDimensions; definition.vector_metric = "l2";
            if (vectorToken) (void)session.ensurePropertyToken("embedding");
            (void)session.createIndexDefinition(definition);
        }
        session.setConfiguration("read_hint", PropertyValue(int64_t(7))); session.finish();
        if (unknown) {
            const std::array<std::byte, 2> key{std::byte{254}, std::byte{1}};
            tx.stageScalarIndexUpsert(trees, RSS_SYSTEM_CATALOG_INDEX_ID, key, 1);
        }
        ASSERT_TRUE(tx.commit().committed()); io->flushStream();
    }
    void setAge(int64_t value, bool expectRefusal = false, bool eraseOwner = false, uint64_t ownerOverride = 0) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file->is_open()); auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io);
        // Writable-open recovery may repair pool bitmaps before the first
        // writer. Complete it through an independently checked metadata-only
        // lease before asserting that schema refusal does no private IO.
        if (expectRefusal) (void)metadata->reserveNativeGraphLogicalIds(1, 0);
        auto old = metadata->openSnapshot();
        const uint64_t owner = ownerOverride ? ownerOverride : eraseOwner ? 64 : 1;
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
    void setEmbedding(const PropertyValue &value, bool refuse) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); (void)metadata->reserveNativeGraphLogicalIds(1, 0);
        auto old = metadata->openSnapshot();
        SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, *metadata); SystemCatalog catalog(trees, *metadata);
        const auto token = catalog.findPropertyToken(old, "embedding"); ASSERT_TRUE(token);
        const auto definitions = catalog.readIndexDefinitions(old, 128);
        const auto definition = std::ranges::find(definitions, IndexDefinitionKind::IDK_VECTOR, &IndexDefinition::kind);
        ASSERT_NE(definition, definitions.end()); const auto index = definition->index_id;
        const auto mapping = metadata->findIndexRoot(old, index);
        const auto manifest = metadata->findVectorManifest(old, index);
        const auto mappingPage = metadata->findPageLocation(old, mapping.root_page_id);
        const auto manifestPage = metadata->findPageLocation(old, manifest.manifest_page_id);
        PropertyCollectionRootStore roots(pages, *metadata);
        const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
        const auto previous = roots.readEntityPropertyOptional(old, owner, *token); ASSERT_TRUE(previous);
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{owner, *token, OwnedPropertyValue(value), false}}};
        const auto before = old.superblock(); const auto epoch = io->dataMutationEpoch();
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(180), 903, nullptr, true};
        if (refuse) {
            EXPECT_THROW((void)metadata->mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
            EXPECT_EQ(metadata->openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
        } else {
            EXPECT_NO_THROW((void)metadata->mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
            auto current = metadata->openSnapshot();
            EXPECT_EQ(metadata->findIndexRoot(current, index), mapping); EXPECT_EQ(metadata->findVectorManifest(current, index), manifest);
            EXPECT_EQ(metadata->findPageLocation(current, mapping.root_page_id), mappingPage);
            EXPECT_EQ(metadata->findPageLocation(current, manifest.manifest_page_id), manifestPage);
            const auto final = roots.readEntityPropertyOptional(current, owner, *token); ASSERT_TRUE(final); EXPECT_EQ(final->value, value);
        }
        const auto retained = roots.readEntityPropertyOptional(old, owner, *token); ASSERT_TRUE(retained);
        EXPECT_EQ(retained->value, previous->value); EXPECT_EQ(memory->used(), 0U); io->flushStream();
    }
    enum class NativeVectorTestAction { NVTA_NONE, NVTA_CANCEL, NVTA_ALL_OWNERS, NVTA_PROBE_STAGING, NVTA_CONSTRAINT_FAILURE, NVTA_SECOND_OWNER, NVTA_ROW_NOOP };
    void coordinateVectorField(const PropertyValue &value, bool erase = false, bool eraseOwner = false,
        bool preparationFailure = false, size_t maximumBytes = 1024 * 1024, MetadataPublishOptions publishOptions = {},
        NativeVectorTestAction action = NativeVectorTestAction::NVTA_NONE) {
        const bool cancelBeforeCommit = action == NativeVectorTestAction::NVTA_CANCEL;
        const bool allOwners = action == NativeVectorTestAction::NVTA_ALL_OWNERS;
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); (void)metadata->reserveNativeGraphLogicalIds(1, 0);
        SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, *metadata); SystemCatalog catalog(trees, *metadata);
        EntityPageStore entities(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityLabelSetStore labels(io, *metadata, pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        VectorPageStore raw(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        VectorManifestStore manifests(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore values(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, values, *metadata); auto old = metadata->openSnapshot();
        const auto token = catalog.findPropertyToken(old, "embedding"); ASSERT_TRUE(token);
        const auto definitions = catalog.readIndexDefinitions(old, 128);
        const auto definition = std::ranges::find(definitions, IndexDefinitionKind::IDK_VECTOR, &IndexDefinition::kind);
        ASSERT_NE(definition, definitions.end()); const auto index = definition->index_id;
        const uint64_t id = eraseOwner ? 64 : action == NativeVectorTestAction::NVTA_SECOND_OWNER ? 2 : 1; const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, id};
        const auto before = roots.readEntityPropertyOptional(old, owner, *token);
        const auto previousCounts = catalog.readGraphCardinalityStatistics(old);
        const auto oldVector = metadata->findIndexRootOptional(old, index) ?
            BTreeIndex(index, trees, *metadata).find(old, VectorSemanticCodec::nodeMappingKey(id)) : std::nullopt;
        std::vector<std::byte> oldRaw;
        if (oldVector) {
            const auto route = metadata->findVectorLocation(old, *oldVector);
            oldRaw = raw.read(old, route.page_id).read(route.slot_id, route.slot_generation);
        }
        struct Cancel final : CancellationCheckpoint {
            bool armed = false, probe = false; NativeGraphWriteTransaction *transaction = nullptr;
            BTreePageStore *trees = nullptr; uint64_t index = 0; mutable size_t rejected = 0, unexpected = 0;
            void check() const override {
                if (armed) throw std::runtime_error("Vector commit cancelled");
                if (probe && transaction) {
                    try { transaction->stageScalarIndexUpsert(*trees, index, VectorSemanticCodec::nodeMappingKey(999), 1); ++unexpected; }
                    catch (const FormatException &) { ++rejected; }
                }
            }
        } cancellation;
        cancellation.probe = action == NativeVectorTestAction::NVTA_PROBE_STAGING;
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(maximumBytes);
        {
            NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(180);
            options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(180), 903};
            NativeGraphWritePlanner planner(*metadata, entities, labels, roots, trees, options); auto tx = planner.beginTransaction();
            std::vector<NativeHybridPropertyColumnReplacement> cells;
            for (uint64_t target = 1; target <= (allOwners ? 64U : 1U); ++target)
                cells.push_back({allOwners ? EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, target} : owner, *token, OwnedPropertyValue(value), erase});
            const std::array<EntityPropertyKey, 1> deleted{owner};
            if (eraseOwner) tx.stageHybridPropertyOwnerDeletion(deleted, memory, &cancellation);
            else tx.stageHybridPropertyColumns(cells, memory, &cancellation, true);
            cancellation.transaction = &tx; cancellation.trees = &trees; cancellation.index = index;
            const auto epoch = io->dataMutationEpoch();
            const NativeGraphVectorDeltaOptions vectorOptions{PhysicalSlotId(40), PhysicalSlotId(180), 128, 64, 1024, 4096, 64};
            if (preparationFailure) {
                if (maximumBytes < 1024 * 1024)
                    EXPECT_THROW(tx.coordinateHybridPropertyVectors(raw, manifests, vectorOptions), query::execution::MaterializedByteLimitExceeded);
                else EXPECT_THROW(tx.coordinateHybridPropertyVectors(raw, manifests, vectorOptions), std::invalid_argument);
                EXPECT_THROW((void)tx.commit(), FormatException);
                auto moved = std::move(tx); NativeGraphWriteTransaction assigned; assigned = std::move(moved);
                EXPECT_THROW((void)assigned.commit(), FormatException); assigned.rollback();
                EXPECT_EQ(metadata->openSnapshot().superblock(), old.superblock()); EXPECT_EQ(io->dataMutationEpoch(), epoch);
                EXPECT_EQ(memory->used(), 0U); return;
            }
            tx.coordinateHybridPropertyVectors(raw, manifests, vectorOptions);
            if (cancelBeforeCommit) {
                cancellation.armed = true; EXPECT_THROW((void)tx.commit(), std::runtime_error);
                EXPECT_EQ(tx.state(), NativeGraphWriteTransactionState::NGWTS_ACTIVE);
                EXPECT_EQ(metadata->openSnapshot().superblock(), old.superblock()); EXPECT_EQ(io->dataMutationEpoch(), epoch);
                cancellation.armed = false;
            }
            // A successful coordinator must retain its account and typed store
            // bindings across both move construction and move assignment.
            auto moved = std::move(tx); NativeGraphWriteTransaction ready; ready = std::move(moved);
            cancellation.transaction = &ready;
            if (action == NativeVectorTestAction::NVTA_CONSTRAINT_FAILURE) {
                bool rejected = false;
                try { (void)ready.commit(); }
                catch (const std::runtime_error &error) {
                    rejected = true; EXPECT_NE(std::string_view(error.what()).find("duplicate key"), std::string_view::npos);
                }
                EXPECT_TRUE(rejected);
                EXPECT_EQ(metadata->openSnapshot().superblock(), old.superblock()); EXPECT_EQ(io->dataMutationEpoch(), epoch);
                EXPECT_EQ(memory->used(), 0U); return;
            }
            if (publishOptions.bootstrap_failpoint != BootstrapPublishFailpoint::BPF_NONE) {
                EXPECT_THROW((void)ready.commit(publishOptions), BootstrapPublishInterrupted);
                EXPECT_EQ(ready.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
                const auto retained = roots.readEntityPropertyOptional(old, owner, *token);
                ASSERT_EQ(bool(retained), bool(before)); if (before) EXPECT_EQ(retained->value, before->value);
                if (oldVector) {
                    const auto route = metadata->findVectorLocation(old, *oldVector);
                    EXPECT_EQ(raw.read(old, route.page_id).read(route.slot_id, route.slot_generation), oldRaw);
                }
                EXPECT_EQ(memory->used(), 0U); io->flushStream(); return;
            }
            const auto result = ready.commit(); ASSERT_TRUE(result.committed());
            if (action == NativeVectorTestAction::NVTA_ROW_NOOP) {
                EXPECT_EQ(result.superblock, old.superblock()); EXPECT_EQ(io->dataMutationEpoch(), epoch);
            } else EXPECT_EQ(result.superblock.generation.value(), old.superblock().generation.value() + 1);
            if (cancellation.probe) { EXPECT_GT(cancellation.rejected, 0U); EXPECT_EQ(cancellation.unexpected, 0U); }
        }
        auto current = metadata->openSnapshot();
        const auto after = roots.readEntityPropertyOptional(current, owner, *token);
        if (eraseOwner || erase) EXPECT_FALSE(after); else { ASSERT_TRUE(after); EXPECT_EQ(after->value, value); }
        if (eraseOwner) EXPECT_FALSE(metadata->findNodePageLocationOptional(current, id));
        const auto vector = BTreeIndex(index, trees, *metadata).find(current, VectorSemanticCodec::nodeMappingKey(id));
        if (eraseOwner || erase) EXPECT_FALSE(vector); else {
            ASSERT_TRUE(vector); if (oldVector) {
                if (action == NativeVectorTestAction::NVTA_ROW_NOOP) EXPECT_EQ(vector, oldVector);
                else EXPECT_NE(vector, oldVector);
            }
            const auto location = metadata->findVectorLocation(current, *vector);
            EXPECT_NO_THROW((void)raw.read(current, location.page_id).read(location.slot_id, location.slot_generation));
        }
        if (oldVector) {
            EXPECT_EQ(bool(metadata->findVectorLocationOptional(current, *oldVector)), action == NativeVectorTestAction::NVTA_ROW_NOOP);
            const auto route = metadata->findVectorLocation(old, *oldVector);
            EXPECT_EQ(raw.read(old, route.page_id).read(route.slot_id, route.slot_generation), oldRaw);
        }
        if (action == NativeVectorTestAction::NVTA_SECOND_OWNER && erase) {
            const auto previousDelta = manifests.read(old, index).manifest().delta_mapping_index_id;
            ASSERT_NE(previousDelta, 0U); EXPECT_TRUE(metadata->findIndexRootOptional(old, previousDelta));
            EXPECT_FALSE(metadata->findIndexRootOptional(current, previousDelta));
            EXPECT_EQ(manifests.read(current, index).manifest().delta_mapping_index_id, 0U);
        }
        const auto retained = roots.readEntityPropertyOptional(old, owner, *token);
        ASSERT_EQ(bool(retained), bool(before)); if (before) EXPECT_EQ(retained->value, before->value);
        if (previousCounts) {
            const auto counts = catalog.readGraphCardinalityStatistics(current); ASSERT_TRUE(counts);
            EXPECT_EQ(counts->node_count, previousCounts->node_count - size_t(eraseOwner)); EXPECT_EQ(counts->edge_count, previousCounts->edge_count);
        }
        if (allOwners) for (uint64_t target = 1; target <= 64; ++target) {
            const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, target};
            const auto cell = roots.readEntityPropertyOptional(current, key, *token); ASSERT_TRUE(cell); EXPECT_EQ(cell->value, value);
            const auto retained = roots.readEntityPropertyOptional(old, key, *token); ASSERT_TRUE(retained);
            EXPECT_EQ(retained->value, PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(target - 1)), PropertyValue(int64_t(1))}));
            const auto oldId = BTreeIndex(index, trees, *metadata).find(old, VectorSemanticCodec::nodeMappingKey(target)); ASSERT_TRUE(oldId);
            EXPECT_FALSE(metadata->findVectorLocationOptional(current, *oldId));
        }
        EXPECT_EQ(memory->used(), 0U);
        auto inspection = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io, *metadata, inspection).scan()); EXPECT_EQ(inspection->used(), 0U);
        io->flushStream();
    }
    NativeBulkRunFragmentLocation makeOrphanScratch(SegmentKind kind) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); PhysicalSegmentAllocator physical(io, *metadata);
        NativeBulkRunStoreOptions options{PhysicalSlotId(40), PhysicalSlotId(180)}; options.scratch_kind = kind;
        NativeBulkRunStore store(physical, *metadata, options);
        const auto before = metadata->openSnapshot().superblock();
        const auto location = store.writeOpaquePayload(71, {std::byte{0x11}}, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        io->flushStream(); return location;
    }
    void applyPublicNativePatch(RelocatableStorageBackend &backend, std::vector<LogicalGraphWriteProperty> sets,
        std::vector<std::string> removals = {}) {
        auto source = backend.openLogicalGraphReadSnapshot(); const auto fence = source->mutationFence(); ASSERT_TRUE(fence);
        const std::array<LogicalGraphPropertyPatchRequest, 1> patches{{{
            {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
            std::move(sets), std::move(removals)}}};
        (void)backend.executeLogicalGraphMutationBatchWithPropertyPatches(*source, {}, patches, 4096, *fence);
    }
    void inspectPublicNativeState() {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::openReadOnly(io);
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        const auto epoch = io->dataMutationEpoch();
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io, *metadata, memory).scan());
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io->dataMutationEpoch(), epoch);
    }
    std::vector<std::pair<uint64_t, float>> exactVectorSearch(std::vector<float> query = {0.0F, 1.0F}) {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open(); auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
        LogicalGraphVectorSearchRequest request; request.index_name = "person_embedding"; request.query_vector = std::move(query);
        request.top_k = 4; request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
        request.maximum_mapping_entries = 128; request.maximum_raw_vector_reads = 128;
        const auto result = snapshot->searchLogicalVector(request); std::vector<std::pair<uint64_t, float>> hits;
        for (const auto &hit : result.hits) hits.emplace_back(hit.node_id, hit.score);
        return hits;
    }
    void convert(EntityPropertyOwnerKind kind, bool inverse = false) {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file->is_open()); auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); const auto before = metadata->openSnapshot().superblock();
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
        NativeGraphHybridPropertyConversion conversion{kind, 1, memory, nullptr, PhysicalSlotId(40), PhysicalSlotId(180), 903};
        conversion.publish_candidate = true;
        const auto *compact = std::getenv("ZYX_TEST_COMPACT_COLUMN_MEMBERSHIPS");
        conversion.compact_column_memberships = compact && std::string_view(compact) == "1";
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
