#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
using namespace relocatable_storage_backend_test;
TEST_F(RelocatableStorageBackendTest, PublicAdaptiveRowStoreAdmitsOnlyTheWholeNativeCohortAndResumesLogicalQueries) {
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    options.maximum_database_file_bytes = uint64_t(2) * 1024 * 1024 * 1024;
    ASSERT_TRUE(options.adaptive_property_row_chunks);
    auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
    for (uint64_t group = 0; group < 8; ++group) {
        std::vector<LogicalGraphNodeCreateRequest> nodes;
        for (uint64_t offset = 1; offset <= 64; ++offset) {
            LogicalGraphNodeCreateRequest node; node.labels.push_back("Owner");
            for (uint64_t field = 1; field <= 64; ++field)
                node.properties.push_back({"p" + std::to_string(field), PropertyValue(int64_t(group * 64 + offset + field))});
            nodes.push_back(std::move(node));
        }
        ASSERT_EQ(backend->createLogicalNodes(nodes).size(), 64U);
    }
    backend->close(); backend.reset();
    {
        auto stream = std::make_shared<std::fstream>(options.data_path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(stream->is_open());
        auto io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); EntityPageStore entities(io, *metadata);
        SlottedPagePool pages(io, *metadata); BTreePageStore trees(pages, *metadata);
        EntityLabelSetStore labels(io, *metadata, pages); PropertyCollectionRootStore roots(pages, *metadata);
        NativeGraphWritePlannerOptions placement; placement.first_slot = PhysicalSlotId(1); placement.last_slot = PhysicalSlotId(16383);
        placement.entity_pool_owner_id = 901; placement.property_roots = {PhysicalSlotId(1), PhysicalSlotId(16383), 903};
        NativeGraphWritePlanner writer(*metadata, entities, labels, roots, trees, placement);
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
        NativeHybridPropertyAllocationAdmission admission; admission.maximum_admitted_peak_bytes = 32U * 1024U * 1024U;
        admission.maximum_admitted_file_bytes = options.maximum_database_file_bytes;
        const auto old = metadata->openSnapshot(); const auto before = old.superblock();
        struct Cost { int64_t row = 0, candidate = 0, passed = 0; } cost;
        {
            graph::debug::PerfTrace::ScopedObserver observe(&cost, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
                auto &c = *static_cast<Cost *>(owner);
                if (key == "native_graph.hybrid.allocation_row_bytes") c.row = value;
                if (key == "native_graph.hybrid.allocation_candidate_bytes") c.candidate = value;
                if (key == "native_graph.hybrid.allocation_admission_passed") c.passed += value;
            });
            auto tx = writer.beginTransaction();
            tx.stageHybridPropertyConversion(EntityPropertyOwnerKind::EPOK_NODE, 1, memory, nullptr, true,
                NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, admission, 8);
            ASSERT_TRUE(tx.commit().committed());
        }
        ASSERT_EQ(cost.passed, 1); EXPECT_LE(cost.candidate, cost.row / 10 * 9 + cost.row % 10 * 9 / 10);
        const auto now = metadata->openSnapshot(); EXPECT_EQ(now.superblock().generation.value(), before.generation.value() + 1);
        for (uint64_t id = 1; id <= 512; ++id) {
            EXPECT_TRUE(metadata->findEntityPropertyLocationOptional(old, {EntityPropertyOwnerKind::EPOK_NODE, id}));
            EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(now, {EntityPropertyOwnerKind::EPOK_NODE, id}));
            EXPECT_EQ(roots.readEntity(old, {EntityPropertyOwnerKind::EPOK_NODE, id}),
                      roots.readEntity(now, {EntityPropertyOwnerKind::EPOK_NODE, id}));
        }
        EXPECT_EQ(memory->used(), 0U);
        std::cout << "PUBLIC_COHORT_COST adaptive_row=true row=" << cost.row << " candidate=" << cost.candidate << '\n';
    }
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    graph::query::QueryEngine engine(backend, backend);
    const auto counts = engine.execute("MATCH (o:Owner) RETURN count(o) AS owners,sum(o.p1) AS first,sum(o.p64) AS last");
    ASSERT_EQ(counts.rowCount(), 1U);
    EXPECT_EQ(counts.getRows()[0].at("owners").asPrimitive(), PropertyValue(int64_t(512)));
    EXPECT_EQ(counts.getRows()[0].at("first").asPrimitive(), PropertyValue(int64_t(131840)));
    EXPECT_EQ(counts.getRows()[0].at("last").asPrimitive(), PropertyValue(int64_t(164096)));
    const auto owning = engine.execute("MATCH (o:Owner) WHERE o.p1=2 RETURN o AS owner");
    ASSERT_EQ(owning.rowCount(), 1U);
    ASSERT_EQ(owning.getRows()[0].at("owner").asNode().getProperties().size(), 64U);
    for (uint64_t field = 1; field <= 64; ++field)
        EXPECT_EQ(owning.getRows()[0].at("owner").asNode().getProperties().at("p" + std::to_string(field)),
                  PropertyValue(int64_t(1 + field)));
    // Exercise the scalar callback itself, rather than the ROW owning fallback.
    for (bool vector : {false, true}) {
        auto transaction = backend->beginLogicalGraphTransaction(false);
        LogicalGraphTransactionOverlay overlay(*transaction);
        auto snapshot = vector ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
            : overlay.openLogicalGraphReadSnapshot();
        ASSERT_TRUE(snapshot->supportsNodePropertyCellBatches());
        const std::array<uint64_t, 1> ids{1}; const std::array<std::string_view, 1> keys{"p1"};
        bool scalarDelivered = false;
        EXPECT_THROW(snapshot->readNodePropertyCellsWithScalarsSortedWithAdmission(ids, keys,
            [](size_t, size_t, std::optional<PropertyValue>) { ADD_FAILURE() << "native column used owning fallback"; },
            [&](size_t row, size_t column, graph::PropertyType type, uint64_t word) {
                scalarDelivered = true; EXPECT_EQ(row, 0U); EXPECT_EQ(column, 0U);
                EXPECT_EQ(type, graph::PropertyType::INTEGER); EXPECT_EQ(word, 2U);
                overlay.replaceLogicalNodeProperties({{1, {{"p1", PropertyValue(999)}}}});
            }, [](size_t) {}), std::runtime_error);
        EXPECT_TRUE(scalarDelivered); EXPECT_FALSE(snapshot->supportsNodePropertyCellBatches());
        EXPECT_EQ(snapshot->readNodeProperty(1, "p1"), PropertyValue(999));
        transaction->rollback();
    }
    const auto changed = engine.execute("MATCH (o:Owner) WHERE o.p1=2 SET o.p64=777 RETURN o.p64 AS value");
    ASSERT_EQ(changed.rowCount(), 1U); EXPECT_EQ(changed.getRows()[0].at("value").asPrimitive(), PropertyValue(int64_t(777)));
    const auto current = engine.execute("MATCH (o:Owner) WHERE o.p1=2 RETURN o.p64 AS value");
    ASSERT_EQ(current.rowCount(), 1U); EXPECT_EQ(current.getRows()[0].at("value").asPrimitive(), PropertyValue(int64_t(777)));
    const auto gather = engine.execute("UNWIND [2,0,2] AS probe OPTIONAL MATCH (o:Owner {p1:probe}) RETURN probe,o.p64 AS value");
    ASSERT_EQ(gather.rowCount(), 3U);
    EXPECT_EQ(gather.getRows()[0].at("value").asPrimitive(), PropertyValue(int64_t(777)));
    EXPECT_EQ(gather.getRows()[1].at("value").asPrimitive(), PropertyValue());
    EXPECT_EQ(gather.getRows()[2].at("value").asPrimitive(), PropertyValue(int64_t(777)));
    backend->close();
    EXPECT_EQ(owning.getRows()[0].at("owner").asNode().getProperties().at("p64"), PropertyValue(int64_t(65)));
#endif
}
