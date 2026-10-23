from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdPublicConversion.cpp');s=p.read_text();s=s.replace('#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"','#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"\n#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"\n#include "graph/query/api/QueryEngine.hpp"')
a=s.index('    void stage(')
s=s[:a]+'''    void catalog(EntityPropertyOwnerKind kind = NODE) {
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
        SystemCatalog system(*btree_pages_, *metadata_); auto tx = writer.beginTransaction(); auto schema = system.beginWrite(tx);
        uint32_t ordinal = 0;
        for (const auto *name : {"id", "age", "country", "score", "description"}) EXPECT_EQ(schema.ensurePropertyToken(name), ++ordinal);
        schema.setGraphCardinalityStatistics({kind == NODE ? 512U : 2U, kind == NODE ? 0U : 512U});
        if (kind == EDGE) for (unsigned type = 1; type <= 7; ++type) {
            const auto token = schema.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "Type" + std::to_string(type));
            EXPECT_EQ(token, type); schema.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, token, type == 7 ? 512 : 0);
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
'''+s[a:]
a=s.index('#else\nTEST_F(')
t='''
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
'''
s=s[:a]+t+s[a:];p.write_text(s)
