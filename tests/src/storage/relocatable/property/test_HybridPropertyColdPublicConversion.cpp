#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/query/api/QueryEngine.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyColdPublicConversionTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    static NativeGraphWritePlannerOptions placement(bool native = false) {
        NativeGraphWritePlannerOptions result; result.first_slot = PhysicalSlotId(40); result.last_slot = PhysicalSlotId(80);
        result.entity_pool_owner_id = 901; result.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), native ? 903U : 902U};
        return result;
    }
    static NativeHybridPropertyAllocationAdmission admission() {
        NativeHybridPropertyAllocationAdmission result;
        result.maximum_admitted_peak_bytes = 32U * 1024U * 1024U;
        result.maximum_admitted_file_bytes = 32U * 1024U * 1024U; return result;
    }
    static PropertyCollectionRoot expected(uint64_t id, EntityPropertyOwnerKind kind = NODE, size_t textBytes = 768) {
        return {id, kind == NODE ? graph::Node::typeId : graph::Edge::typeId,
            {{1, PropertyValue("user-" + std::to_string(100000 + id))}, {2, PropertyValue(int64_t(id % 80))},
             {3, PropertyValue(id % 2 ? "US" : "DE")}, {4, PropertyValue(double(id))},
             {5, PropertyValue(std::string(textBytes, 'd'))}}};
    }
    void prepare(EntityPropertyOwnerKind kind = NODE, size_t groups = 8, size_t textBytes = 768, bool spareLast = false) {
        const auto count = groups * 64 - size_t(spareLast);
        seed(kind, count, 0);
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
        for (size_t group = 0; group < groups; ++group) {
            auto tx = writer.beginTransaction();
            for (size_t i = 1; i <= 64; ++i) {
                if (group * 64 + i > count) break;
                auto row = expected(group * 64 + i, kind, textBytes);
                if (kind == NODE) tx.replaceNodeProperty(std::move(row)); else tx.replaceEdgeProperty(std::move(row));
            }
            ASSERT_TRUE(tx.commit().committed());
        }
    }
    void catalog(EntityPropertyOwnerKind kind = NODE, uint64_t count = 512) {
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
        SystemCatalog system(*btree_pages_, *metadata_); auto tx = writer.beginTransaction(); auto schema = system.beginWrite(tx);
        uint32_t ordinal = 0;
        for (const auto *name : {"id", "age", "country", "score", "description"}) EXPECT_EQ(schema.ensurePropertyToken(name), ++ordinal);
        schema.setGraphCardinalityStatistics({kind == NODE ? count : 2U, kind == NODE ? 0U : count});
        if (kind == EDGE) for (unsigned type = 1; type <= 7; ++type) {
            const auto token = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "Type" + std::to_string(type));
            EXPECT_EQ(token, type); schema.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, token, type == 7 ? count : 0);
        }
        schema.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    void convert(const std::shared_ptr<MaterializedByteBudget> &memory, EntityPropertyOwnerKind kind = NODE) {
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
        auto tx = writer.beginTransaction(); stage(tx, memory, kind); ASSERT_TRUE(tx.commit().committed());
        const auto pin = metadata_->openSnapshot(); ASSERT_TRUE(pin.superblock().hybrid_property_descriptor_root.isValid());
        ASSERT_FALSE(metadata_->findEntityPropertyLocationOptional(pin, {kind, 1}));
    }
    void verify(const MetadataSnapshotPin &pin, const std::shared_ptr<MaterializedByteBudget> &memory,
        EntityPropertyOwnerKind kind = NODE, size_t textBytes = 768) {
        graph::utils::OwnedAllocationScope scope(memory);
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
        for (uint64_t id = 1; id <= 512; ++id) EXPECT_EQ(roots.readEntity(pin, {kind, id}), expected(id, kind, textBytes));
    }
    void stage(NativeGraphWriteTransaction &tx, const std::shared_ptr<MaterializedByteBudget> &memory,
        EntityPropertyOwnerKind kind = NODE, uint8_t groups = 8, const CancellationCheckpoint *cancellation = nullptr) {
        tx.stageHybridPropertyConversion(kind, 1, memory, cancellation, true,
            NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, admission(), groups, true);
    }
};
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT && defined(ZYX_HAS_LZ4)
TEST_F(HybridPropertyColdPublicConversionTest, PublicColdCohortUsesRealRowSourcesAndTheWholeDatabaseGate) {
    prepare(); auto old = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    struct Costs { int64_t row = 0, candidate = 0, accepted = 0, refused = 0; } costs;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&costs, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            auto &c = *static_cast<Costs *>(p);
            if (key == "native_graph.hybrid.allocation_row_bytes") c.row = value;
            if (key == "native_graph.hybrid.allocation_candidate_bytes") c.candidate = value;
            if (key == "native_graph.hybrid.allocation_admission_passed") c.accepted += value;
            if (key == "native_graph.hybrid.allocation_admission_insufficient_saving") c.refused += value;
        });
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
        auto tx = writer.beginTransaction(); stage(tx, memory); ASSERT_TRUE(tx.commit().committed());
    }
    RecordProperty("row_steady_bytes", std::to_string(costs.row)); RecordProperty("candidate_steady_bytes", std::to_string(costs.candidate));
    ASSERT_GT(costs.row, 0); ASSERT_EQ(costs.accepted, 1); EXPECT_EQ(costs.refused, 0);
    EXPECT_LE(costs.candidate, costs.row - (costs.row + 9) / 10);
    auto current = metadata_->openSnapshot(); ASSERT_NE(current.superblock(), old.superblock());
    {
    graph::utils::OwnedAllocationScope scope(memory);
    for (uint64_t id = 1; id <= 512; ++id) {
        EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(current, {NODE, id}));
        EXPECT_EQ(properties_->readEntity(current, {NODE, id}), expected(id));
        EXPECT_EQ(properties_->readEntity(old, {NODE, id}), expected(id));
    }
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyColdPublicConversionTest, UnprofitableColdCohortNeverInstallsAnEarlierGroup) {
    prepare(NODE, 8, 0); const auto old = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); int64_t refused = 0;
    {
        graph::debug::PerfTrace::ScopedObserver observer(&refused, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.allocation_admission_insufficient_saving") *static_cast<int64_t *>(p) += value;
        });
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
        auto tx = writer.beginTransaction(); stage(tx, memory); ASSERT_TRUE(tx.commit().committed());
    }
    EXPECT_EQ(refused, 1); EXPECT_EQ(metadata_->openSnapshot().superblock(), old.superblock()); verify(old, memory, NODE, 0);
    auto current = metadata_->openSnapshot(); for (uint64_t id = 1; id <= 512; ++id) EXPECT_TRUE(metadata_->findEntityPropertyLocationOptional(current, {NODE, id}));
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdPublicConversionTest, PublicEdgeConversionPreservesActualEdgeAuthorityAndAllValues) {
    prepare(EDGE); catalog(EDGE); auto old = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); convert(memory, EDGE);
    auto current = metadata_->openSnapshot(); verify(current, memory, EDGE); verify(old, memory, EDGE);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdPublicConversionTest, RealConvertedRowsSupportProductionFilterGroupAndColdPointAfterReopen) {
    prepare(); catalog(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); convert(memory);
    RelocatableStorageBackendOptions options; options.data_path = path_; options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open(); graph::query::QueryEngine engine(backend);
    auto filtered = engine.execute("MATCH (n) WHERE n.country='US' RETURN count(n) AS rows"); ASSERT_EQ(filtered.rowCount(), 1U);
    EXPECT_EQ(filtered.getRows().front().at("rows").asPrimitive(), PropertyValue(int64_t(256)));
    auto grouped = engine.execute("MATCH (n) RETURN n.country AS country, count(n) AS rows"); ASSERT_EQ(grouped.rowCount(), 2U);
    for (const auto &row : grouped.getRows()) EXPECT_EQ(row.at("rows").asPrimitive(), PropertyValue(int64_t(256)));
    auto cold = engine.execute("MATCH (n) WHERE n.id='user-100512' RETURN n.description AS description"); ASSERT_EQ(cold.rowCount(), 1U);
    EXPECT_EQ(cold.getRows().front().at("description").asPrimitive(), PropertyValue(std::string(768, 'd')));
    backend->close(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdPublicConversionTest, ConvertedHotDemotionAndLargeColdWriteRetainOriginalRowsAndOtherGroups) {
    prepare(); catalog(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); convert(memory);
    auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 2, false},
        {{NODE, 1}, 5, graph::OwnedPropertyValue(PropertyValue(std::string(8193, 'x')))}}};
    NativeHybridPropertyColumnWrite write{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903}; write.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
        for (uint64_t id = 1; id <= 512; ++id) {
            auto wanted = expected(id); if (id == 1) { wanted.entries[1].value = PropertyValue(false); wanted.entries[4].value = PropertyValue(std::string(8193, 'x')); }
            EXPECT_EQ(roots.readEntity(current, {NODE, id}), wanted); EXPECT_EQ(roots.readEntity(old, {NODE, id}), expected(id));
        }
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdPublicConversionTest, PublicInverseRetainsSevenConvertedGroupsAndOldPins) {
    prepare(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); convert(memory); auto old = metadata_->openSnapshot();
    auto inverse = request(NODE, memory); inverse.conversion->publish_candidate = true;
    inverse.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW; run(inverse);
    auto current = metadata_->openSnapshot();
    for (uint64_t id = 1; id <= 512; ++id) EXPECT_EQ(metadata_->findEntityPropertyLocationOptional(current, {NODE, id}).has_value(), id <= 64);
    verify(current, memory); verify(old, memory);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdPublicConversionTest, PublicColdCancellationAfterCompleteCostPreparationHasNoVisiblePrefixAndRetries) {
    struct Cancel final : CancellationCheckpoint { bool prepared = false; void check() const override { if (prepared) throw std::runtime_error("cancel cold admission"); } } cancel;
    prepare(); auto old = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancel, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.allocation_costs_prepared" && value) static_cast<Cancel *>(p)->prepared = true;
        });
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
        auto tx = writer.beginTransaction(); stage(tx, memory, NODE, 8, &cancel); EXPECT_THROW((void)tx.commit(), std::runtime_error); tx.rollback();
    }
    ASSERT_TRUE(cancel.prepared); EXPECT_EQ(metadata_->openSnapshot().superblock(), old.superblock()); verify(old, memory); EXPECT_EQ(memory->used(), 0U);
    cancel.prepared = false; convert(memory); auto current = metadata_->openSnapshot(); verify(current, memory); verify(old, memory);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdPublicConversionTest, PartialAndCompletePublicationRecoverAllColdGroupsOrNone) {
    prepare(); auto old = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
        auto tx = writer.beginTransaction(); stage(tx, memory); MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)tx.commit(options), BootstrapPublishInterrupted); tx.rollback();
        auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *reopened); auto pin = reopened->openSnapshot(); const bool installed = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        {
            graph::utils::OwnedAllocationScope scope(memory);
            for (uint64_t id = 1; id <= 512; ++id) {
                EXPECT_EQ(reopened->findEntityPropertyLocationOptional(pin, {NODE, id}).has_value(), !installed);
                EXPECT_EQ(roots.readEntity(pin, {NODE, id}), expected(id));
            }
        }
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}

TEST_F(HybridPropertyColdPublicConversionTest, ActualConvertedPartialGroupSupportsSealedBirthAndDeletionThroughTheBackend) {
    prepare(NODE, 8, 768, true); catalog(NODE, 511);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); convert(memory);
    RelocatableStorageBackendOptions options; options.data_path = path_; options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    options.verify_integrity_on_open = true;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    const auto ids = backend->createLogicalNodes({{{}, {{"id", PropertyValue("user-100512")}, {"age", PropertyValue()},
        {"country", PropertyValue("US")}, {"score", PropertyValue(512.0)}, {"description", PropertyValue(std::string(8193, 'b'))}}}});
    ASSERT_EQ(ids, std::vector<uint64_t>{512});
    {
        auto snapshot = backend->openLogicalGraphReadSnapshot(); const auto node = snapshot->readNode(512);
        const auto description = std::ranges::find(node.properties, std::string("description"), &LogicalGraphPropertyEntry::key);
        ASSERT_NE(description, node.properties.end()); EXPECT_EQ(description->value, PropertyValue(std::string(8193, 'b')));
    }
    backend->deleteLogicalNodes({{512, false, 0}});
    {
        graph::query::QueryEngine engine(backend); auto count = engine.execute("MATCH (n) RETURN count(n) AS rows");
        ASSERT_EQ(count.rowCount(), 1U); EXPECT_EQ(count.getRows().front().at("rows").asPrimitive(), PropertyValue(int64_t(511)));
    }
    backend->close(); backend->open(); backend->close(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdPublicConversionTest, ForwardColdConversionPublishesItsNewWholeRootValueAndManifestTogether) {
    prepare(); catalog();
    {
        LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, roots, *btree_pages_, placement());
        auto row = expected(1); row.entries[4].value = PropertyValue(std::string(8193, 'l'));
        auto tx = writer.beginTransaction(); tx.replaceNodeProperty(std::move(row)); ASSERT_TRUE(tx.commit().committed());
    }
    auto old = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); convert(memory);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
        for (const auto *pin : {&old, &current}) {
            auto row = expected(1); row.entries[4].value = PropertyValue(std::string(8193, 'l'));
            EXPECT_EQ(roots.readEntity(*pin, {NODE, 1}), row);
            EXPECT_EQ(roots.readEntity(*pin, {NODE, 512}), expected(512));
        }
    }
    {
        graph::utils::OwnedAllocationScope scope(memory);
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueReachabilityReclaimOptions options; options.maximum_manifest_locations = 32;
        options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 32;
        options.maximum_owner_locations = 512; options.maximum_owner_metadata_pages = 512;
        options.maximum_data_pages = 64; options.maximum_retirements = 64; options.maximum_page_locations = 512;
        options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
        LargeValueReachabilityReclaimer gc(values, *slotted_pages_, *metadata_);
        const auto nextLsn = [&] { return LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1); };
        const auto retired = gc.reclaim(options, nextLsn()); EXPECT_EQ(retired.reachable_values, 1U); EXPECT_EQ(retired.retired_values, 1U);
        PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
        auto wanted = expected(1); wanted.entries[4].value = PropertyValue(std::string(8193, 'l'));
        EXPECT_EQ(roots.readEntity(old, {NODE, 1}), wanted); old.release();
        const auto released = gc.reclaim(options, nextLsn()); EXPECT_EQ(released.retired_values, 0U);
        auto current = metadata_->openSnapshot(); EXPECT_EQ(roots.readEntity(current, {NODE, 1}), wanted);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdPublicConversionTest, MissingAdmissionAndInsufficientPeakAllowanceCannotPublishColdRows) {
    prepare(); const auto old = metadata_->openSnapshot(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
    {
        auto tx = writer.beginTransaction();
        EXPECT_THROW(tx.stageHybridPropertyConversion(NODE, 1, memory, nullptr, true,
            NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, {}, 8, true), FormatException);
        tx.rollback();
    }
    {
        auto tx = writer.beginTransaction(); auto limits = admission(); limits.maximum_admitted_peak_bytes = 1;
        tx.stageHybridPropertyConversion(NODE, 1, memory, nullptr, true,
            NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, limits, 8, true);
        ASSERT_TRUE(tx.commit().committed());
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), old.superblock()); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    verify(old, memory); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyColdPublicConversionTest, RealConvertedFieldPoolMovesWithoutBreakingOtherGroupsOrPinnedRows) {
    prepare(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); convert(memory);
    auto old = metadata_->openSnapshot(); CatalogEntry pool;
    metadata_->visitCatalogEntries(old, [&](const auto &entry) {
        if (entry.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && entry.owner_id == 903 && entry.state == SegmentState::SGS_SEALED) pool = entry;
    });
    ASSERT_TRUE(pool.segment_id.isValid()); const auto source = old.superblock();
    NativeHybridPropertyPoolRelocation move{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        pool.segment_id, PhysicalSlotId(80), PhysicalSlotId(80), memory};
    static_cast<void>(metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        auto current = metadata_->openSnapshot(); verify(current, memory); verify(old, memory);
        EXPECT_EQ(metadata_->findCatalog(current, pool.segment_id).state, SegmentState::SGS_RETIRED);
    }
    const auto nextLsn = [&] { return LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1); };
    static_cast<void>(metadata_->reclaimRetiredPhysicalSlots(nextLsn()));
    { auto current = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->isPhysicalSlotAllocated(current, pool.physical_slot)); }
    old.release(); static_cast<void>(metadata_->reclaimRetiredPhysicalSlots(nextLsn()));
    { auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(current, pool.physical_slot)); verify(current, memory); }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#elif !ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyColdPublicConversionTest, DefaultFormatRejectsColdPublicationBeforeIo) {
    seed(NODE, 2, 0); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
    auto tx = writer.beginTransaction(); EXPECT_THROW(stage(tx, memory), FormatException); tx.rollback();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
#else
TEST_F(HybridPropertyColdPublicConversionTest, RawColdLayoutRetainsRowWhenWholeDatabaseSavingIsInsufficient) {
    prepare(); auto old = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
    auto tx = writer.beginTransaction(); stage(tx, memory); ASSERT_TRUE(tx.commit().committed());
    EXPECT_EQ(metadata_->openSnapshot().superblock(), old.superblock()); verify(old, memory); EXPECT_EQ(memory->used(), 0U);
}
#endif
