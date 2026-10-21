#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyOwnerInsertionPublicationTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    file_handle_t native_handle_ = INVALID_FILE_HANDLE;
    std::unique_ptr<LargeValueStore> large_values_;
    void resetStores() {
        vector_manifests_.reset(); vector_pages_.reset(); properties_.reset(); large_values_.reset(); labels_.reset();
        btree_pages_.reset(); slotted_pages_.reset(); entities_.reset(); metadata_.reset(); io_.reset();
    }
    void SetUp() override {
        HybridPropertyOrdinaryConversionTestFixture::SetUp();
        resetStores(); native_handle_ = portable_open_rw(path_.string().c_str());
        ASSERT_NE(native_handle_, INVALID_FILE_HANDLE);
        io_ = std::make_shared<StorageIO>(file_, native_handle_, native_handle_);
        metadata_ = RelocatableMetadataStore::open(io_);
        entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
        slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
        btree_pages_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
        labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
        properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
        vector_pages_ = std::make_unique<VectorPageStore>(io_, *metadata_);
        vector_manifests_ = std::make_unique<VectorManifestStore>(io_, *metadata_);
    }
    void TearDown() override {
        resetStores();
        if (native_handle_ != INVALID_FILE_HANDLE) { portable_close_rw(native_handle_); native_handle_ = INVALID_FILE_HANDLE; }
        HybridPropertyOrdinaryConversionTestFixture::TearDown();
    }
    enum class DerivedCase { ODC_NONE, ODC_VECTOR, ODC_UNIQUE, ODC_REQUIRED, ODC_INTEGER };
    struct CreationCase {
        EntityPropertyOwnerKind kind;
        std::optional<bool> completeFault;
        bool withProperties = false;
        PropertyValue initial = PropertyValue(int64_t(100));
        size_t owners = 63;
        bool extensionField = false;
        bool withCatalog = false;
        bool allocateName = false;
        bool withIndex = false;
        DerivedCase derived = DerivedCase::ODC_NONE;
        bool expectFailure = false;
        bool bothKinds = false;
        bool deleteExisting = false;
        bool updateExisting = false;
        PropertyValue existingValue = PropertyValue(int64_t(900));
    };
    void exercise(const CreationCase &testCase) {
        const auto kind = testCase.kind;
        const auto completeFault = testCase.completeFault;
        const auto withProperties = testCase.withProperties;
        const auto &initial = testCase.initial;
        const auto owners = testCase.owners;
        const uint64_t birthOwner = (owners / 64U + 1U) * 64U;
        const auto extensionField = testCase.extensionField;
        const auto withCatalog = testCase.withCatalog;
        const auto allocateName = testCase.allocateName;
        const auto withIndex = testCase.withIndex;
        const bool vector = testCase.derived == DerivedCase::ODC_VECTOR;
        const bool constraint = testCase.derived == DerivedCase::ODC_UNIQUE || testCase.derived == DerivedCase::ODC_REQUIRED || testCase.derived == DerivedCase::ODC_INTEGER;
        if (testCase.bothKinds) { seed(EDGE, 63, 1); seed(NODE, 61, 1); }
        else seed(kind, owners, 1);
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        uint64_t propertyIndex = 0, supportIndex = 0;
        if (withCatalog) {
            NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
            options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
            NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
            SystemCatalog catalog(*btree_pages_, *metadata_); auto tx = writer.beginTransaction(); auto session = catalog.beginWrite(tx);
            ASSERT_EQ(session.ensurePropertyToken("p"), 1U);
            if (!allocateName) ASSERT_EQ(session.ensurePropertyToken("q"), 2U);
            if (kind == EDGE) {
                for (size_t token = 1; token < 7; ++token) (void)session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "unused" + std::to_string(token));
                ASSERT_EQ(session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "KNOWS"), 7U);
                session.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 7, owners);
                session.markTokenCardinalityComplete(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE);
            }
            if (withIndex || vector) {
                IndexDefinition definition; definition.name = "owner_p";
                definition.kind = vector ? IndexDefinitionKind::IDK_VECTOR : IndexDefinitionKind::IDK_PROPERTY;
                if (vector) { definition.vector_dimensions = 2; definition.vector_metric = "l2"; }
                definition.entity_kind = kind == NODE ? IndexDefinitionEntityKind::IDEK_NODE : IndexDefinitionEntityKind::IDEK_EDGE;
                definition.label_token_id = kind == NODE ? session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Indexed") : 0;
                definition.property_names = {"p"}; definition = session.createIndexDefinition(definition); propertyIndex = definition.index_id;
                std::vector<BTreePageEntry> entries;
                if (kind == EDGE) for (uint64_t id = 1; id <= owners; ++id) {
                    const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(id)), id); BTreePageEntry entry;
                    entry.key.assign(key.begin(), key.end()); entry.value_or_child = id; entries.push_back(std::move(entry));
                }
                if (!vector) tx.stageScalarIndexCreate(*btree_pages_, propertyIndex, entries);
            }
            if (constraint) {
                ConstraintDefinition definition; definition.name = "owner_constraint";
                definition.entity_kind = kind == NODE ? ConstraintDefinitionEntityKind::CDEK_NODE : ConstraintDefinitionEntityKind::CDEK_EDGE;
                definition.kind = testCase.derived == DerivedCase::ODC_UNIQUE ? ConstraintDefinitionKind::CDK_UNIQUE :
                    testCase.derived == DerivedCase::ODC_REQUIRED ? ConstraintDefinitionKind::CDK_NOT_NULL : ConstraintDefinitionKind::CDK_PROPERTY_TYPE;
                definition.schema_token_id = kind == NODE ? session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Indexed") : 7;
                definition.property_names = {"p"};
                if (testCase.derived == DerivedCase::ODC_INTEGER) definition.property_type = ConstraintDefinitionPropertyType::CDPT_INTEGER;
                definition = session.createConstraintDefinition(definition);
                if (definition.kind == ConstraintDefinitionKind::CDK_UNIQUE) {
                    supportIndex = definition.constraint_id; std::vector<BTreePageEntry> entries;
                    if (kind == EDGE) for (uint64_t id = 1; id <= owners; ++id) {
                        const std::array<PropertyValue, 1> values{PropertyValue(int64_t(id))};
                        const auto key = ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), id);
                        BTreePageEntry entry; entry.key.assign(key.begin(), key.end()); entry.value_or_child = id; entries.push_back(std::move(entry));
                    }
                    std::ranges::sort(entries, {}, &BTreePageEntry::key);
                    tx.stageScalarIndexCreate(*btree_pages_, supportIndex, entries);
                }
            }
            session.setGraphCardinalityStatistics({kind == NODE ? owners : 2U, kind == EDGE ? owners : 0U});
            session.finish(); ASSERT_TRUE(tx.commit().committed());
        }
        for (uint64_t group = 1; group <= (birthOwner - 1U) / 64U + 1U; ++group) {
            auto conversion = request(kind, memory); conversion.conversion->group_id = group;
            conversion.conversion->publish_candidate = true; run(conversion);
        }
        if (testCase.bothKinds) { auto other = request(EDGE, memory); other.conversion->publish_candidate = true; run(other); }
        properties_.reset(); labels_.reset(); btree_pages_.reset(); slotted_pages_.reset(); entities_.reset();
        entities_ = std::make_unique<EntityPageStore>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        btree_pages_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
        labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        large_values_ = std::make_unique<LargeValueStore>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *large_values_, *metadata_);
        NativeGraphWritePlannerOptions placement; placement.first_slot = PhysicalSlotId(40); placement.last_slot = PhysicalSlotId(80);
        placement.entity_pool_owner_id = 901; placement.logical_id_lease_block_size = 1;
        placement.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement);
        PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
        NativeGraphMutationTokenResolvers tokens;
        tokens.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind type, std::string_view name) -> std::optional<uint64_t> {
            if (type == SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE && name == "KNOWS") return 7; return {};
        };
        tokens.property_key_token = [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> { return name == "p" ? std::optional<uint32_t>(1) : name == "q" ? std::optional<uint32_t>(2) : std::nullopt; };
        tokens.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> { return token == 1 ? std::optional<std::string>("p") : token == 2 ? std::optional<std::string>("q") : std::nullopt; };
        SystemCatalog catalog(*btree_pages_, *metadata_);
        NativeGraphMutationExecutor executor(planner, graph, *btree_pages_, *vector_pages_, *vector_manifests_, *metadata_, tokens,
            [&](const MetadataSnapshotPin &pin) { return withCatalog ? catalog.readIndexDefinitions(pin, 64) : ::graph::utils::OwnedStdVector<IndexDefinition>{}; },
            [&](const MetadataSnapshotPin &pin) { return withCatalog ? catalog.readConstraintDefinitions(pin, 64) : ::graph::utils::OwnedStdVector<ConstraintDefinition>{}; },
            {(std::max)(size_t(64), owners + 1U), (std::max)(size_t(64), owners + 1U), 128, 64, 128, 128, 64, 128, 4}, withCatalog ? &catalog : nullptr);
        const auto old = metadata_->openSnapshot(); LogicalGraphMutationBatch batch;
        for (size_t index = owners; index < birthOwner; ++index) {
            if (kind == NODE) {
                batch.node_creates.push_back({(withIndex || vector || constraint) ? std::vector<std::string>{"Indexed"} : std::vector<std::string>{}, {}});
                if (withProperties) batch.node_creates.back().properties.push_back({extensionField ? "q" : "p", initial});
            } else {
                batch.edge_creates.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
                    {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2}, "KNOWS", {}});
                if (withProperties) batch.edge_creates.back().properties.push_back({extensionField ? "q" : "p", initial});
            }
        }
        if (testCase.deleteExisting) {
            if (kind == NODE) batch.node_deletes.push_back({1, false, 0});
            else batch.edge_deletes.push_back({1});
        }
        if (testCase.bothKinds) {
            batch.edge_creates.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
                {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2}, "KNOWS", {}});
            if (withProperties) batch.edge_creates.back().properties.push_back({"p", PropertyValue(int64_t(101))});
        }
        if (testCase.updateExisting) batch.property_replacements.push_back({
            {kind == NODE ? LogicalGraphMutationEntityKind::LGMK_NODE : LogicalGraphMutationEntityKind::LGMK_EDGE,
                LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {{"p", testCase.existingValue}}});
        const uint32_t birthToken = extensionField ? 2U : 1U;
        if (testCase.expectFailure) {
            try { (void)executor.execute(batch); FAIL() << "invalid constrained native creation committed"; }
            catch (const std::exception &error) { EXPECT_NE(std::string(error.what()).find("Constraint violation"), std::string::npos) << error.what(); }
            const auto after = metadata_->openSnapshot();
            EXPECT_FALSE(properties_->isHybridPropertyOwner(after, {kind, birthOwner}));
            EXPECT_FALSE(kind == NODE ? bool(metadata_->findNodePageLocationOptional(after, birthOwner)) : bool(metadata_->findEdgePageLocationOptional(after, birthOwner)));
            EXPECT_EQ(HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, after).groupRecord({kind, birthOwner})->live,
                HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, old).groupRecord({kind, birthOwner})->live);
            if (allocateName) EXPECT_FALSE(catalog.findPropertyToken(after, "q"));
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); return;
        }
        if (completeFault) {
            size_t headers = 0; bool injected = false;
            {
                StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) -> pwrite_ssize_t {
                    // One durable lease publication precedes the graph publication.
                    if (offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES && count == RSS_SUPERBLOCK_BYTES && ++headers == 2U) {
                        injected = true;
                        if (!*completeFault) return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_BYTES / 2U, offset);
                        (void)portable_pwrite(fd, bytes, count, offset);
                        throw std::runtime_error("injected owner publication interruption");
                    }
                    return portable_pwrite(fd, bytes, count, offset);
                });
                EXPECT_THROW((void)executor.execute(batch), std::runtime_error);
            }
            ASSERT_TRUE(injected); EXPECT_FALSE(properties_->isHybridPropertyOwner(old, {kind, birthOwner}));
            auto reopened = RelocatableMetadataStore::openReadOnly(io_);
            SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            LargeValueStore values(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            PropertyCollectionRootStore roots(pages, values, *reopened); auto recovered = reopened->openSnapshot();
            HybridPropertyReadContext recoveredFields(io_, pages, *reopened, recovered);
            if (allocateName) {
                BTreePageStore trees(pages, *reopened); SystemCatalog recoveredCatalog(trees, *reopened);
                EXPECT_EQ(recoveredCatalog.findPropertyToken(recovered, "q"), *completeFault ? std::optional<uint32_t>(2) : std::nullopt);
                EXPECT_FALSE(catalog.findPropertyToken(old, "q"));
            }
            if (testCase.updateExisting) EXPECT_EQ(roots.readEntityPropertyOptional(recovered, {kind, 1}, 1)->value,
                *completeFault ? testCase.existingValue : properties_->readEntityPropertyOptional(old, {kind, 1}, 1)->value);
            if (testCase.deleteExisting) {
                EXPECT_EQ(bool(kind == NODE ? reopened->findNodePageLocationOptional(recovered, 1) : reopened->findEdgePageLocationOptional(recovered, 1)), !*completeFault);
                EXPECT_TRUE(properties_->isHybridPropertyOwner(old, {kind, 1}));
            }
            EXPECT_EQ(roots.isHybridPropertyOwner(recovered, {kind, birthOwner}), *completeFault);
            EXPECT_EQ(bool(kind == NODE ? reopened->findNodePageLocationOptional(recovered, birthOwner) :
                reopened->findEdgePageLocationOptional(recovered, birthOwner)), *completeFault);
            EXPECT_EQ(bool(recoveredFields.groupRecord({kind, birthOwner})->live & (UINT64_C(1) << 63)), *completeFault);
            if (withProperties && *completeFault) EXPECT_EQ(roots.readEntityPropertyOptional(recovered, {kind, birthOwner}, birthToken)->value, initial);
            else EXPECT_FALSE(roots.readEntityOptional(recovered, {kind, birthOwner}));
            if (vector) {
                EXPECT_EQ(bool(reopened->findVectorManifestOptional(recovered, propertyIndex)), *completeFault);
                EXPECT_EQ(bool(reopened->findVectorLocationOptional(recovered, 1)), *completeFault);
                if (*completeFault) {
                    BTreePageStore trees(pages, *reopened); VectorManifestStore manifests(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
                    EXPECT_EQ(VectorNodeMapping::findNode(recovered, trees, *reopened, manifests.read(recovered, propertyIndex).manifest().vector_node_mapping_index_id, 1), std::optional<uint64_t>(birthOwner));
                }
            }
            if (supportIndex) {
                BTreePageStore trees(pages, *reopened); const std::array<PropertyValue, 1> values{initial};
                const auto key = ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), birthOwner);
                EXPECT_EQ(BTreeIndex(supportIndex, trees, *reopened).find(recovered, key), *completeFault ? std::optional<uint64_t>(birthOwner) : std::nullopt);
            }
            if (withIndex) {
                BTreePageStore trees(pages, *reopened); const auto key = IndexKeyCodec::encodeEntryKey(initial, birthOwner);
                EXPECT_EQ(BTreeIndex(propertyIndex, trees, *reopened).find(recovered, key), *completeFault ? std::optional<uint64_t>(birthOwner) : std::nullopt);
            }
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan());
            recovered.release();
            // The same writable handle must recover candidate frames and make
            // progress after either publication interruption, with old pins held.
            for (auto &node : batch.node_creates) node.properties.clear();
            for (auto &edge : batch.edge_creates) edge.properties.clear();
            if (testCase.deleteExisting) { batch.node_deletes.clear(); batch.edge_deletes.clear(); }
            EXPECT_TRUE(executor.execute(batch).committed());
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
            return;
        }
        const auto created = executor.execute(batch); ASSERT_TRUE(created.committed());
        const auto &ids = kind == NODE ? created.result.created_node_ids : created.result.created_edge_ids;
        ASSERT_EQ(ids.size(), birthOwner - owners); ASSERT_EQ(ids.front(), owners + 1U); ASSERT_EQ(ids.back(), birthOwner);
        const auto born = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, born);
        if (testCase.bothKinds) {
            ASSERT_EQ(created.result.created_edge_ids, (std::vector<uint64_t>{birthOwner}));
            EXPECT_TRUE(properties_->isHybridPropertyOwner(born, {EDGE, birthOwner}));
            EXPECT_FALSE(properties_->isHybridPropertyOwner(old, {EDGE, birthOwner}));
            EXPECT_EQ(HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, born).groupRecord({EDGE, birthOwner})->live, UINT64_MAX);
            if (withProperties) EXPECT_EQ(properties_->readEntityPropertyOptional(born, {EDGE, birthOwner}, 1)->value, PropertyValue(int64_t(101)));
        }
        if (testCase.updateExisting) {
            EXPECT_EQ(properties_->readEntityPropertyOptional(born, {kind, 1}, 1)->value, testCase.existingValue);
            EXPECT_NE(properties_->readEntityPropertyOptional(old, {kind, 1}, 1)->value, testCase.existingValue);
        }
        ASSERT_TRUE(context.groupRecord({kind, birthOwner})); EXPECT_EQ(context.groupRecord({kind, birthOwner})->live, testCase.deleteExisting ? (UINT64_MAX & ~UINT64_C(1)) : UINT64_MAX);
        if (testCase.deleteExisting) {
            EXPECT_FALSE(kind == NODE ? bool(metadata_->findNodePageLocationOptional(born, 1)) : bool(metadata_->findEdgePageLocationOptional(born, 1)));
            EXPECT_TRUE(properties_->isHybridPropertyOwner(old, {kind, 1}));
        }
        EXPECT_TRUE(properties_->isHybridPropertyOwner(born, {kind, birthOwner}));
        if (withProperties) EXPECT_EQ(properties_->readEntityPropertyOptional(born, {kind, birthOwner}, birthToken)->value, initial);
        else EXPECT_FALSE(properties_->readEntityOptional(born, {kind, birthOwner}));
        EXPECT_FALSE(properties_->isHybridPropertyOwner(old, {kind, birthOwner}));
        if (withIndex) {
            const auto key = IndexKeyCodec::encodeEntryKey(initial, birthOwner);
            EXPECT_EQ(BTreeIndex(propertyIndex, *btree_pages_, *metadata_).find(born, key), std::optional<uint64_t>(birthOwner));
            EXPECT_FALSE(BTreeIndex(propertyIndex, *btree_pages_, *metadata_).find(old, key));
        }
        if (allocateName) {
            EXPECT_EQ(catalog.findPropertyToken(born, "q"), std::optional<uint32_t>(2));
            EXPECT_EQ(catalog.findPropertyName(born, 2), std::optional<std::string>("q"));
            EXPECT_FALSE(catalog.findPropertyToken(old, "q"));
        }
        if (withCatalog) {
            const auto stats = catalog.readGraphCardinalityStatistics(born); ASSERT_TRUE(stats);
            EXPECT_EQ(stats->node_count, kind == NODE ? birthOwner : 2U); EXPECT_EQ(stats->edge_count, kind == EDGE ? birthOwner : 0U);
            const auto oldStats = catalog.readGraphCardinalityStatistics(old); ASSERT_TRUE(oldStats);
            EXPECT_EQ(oldStats->node_count, kind == NODE ? owners : 2U); EXPECT_EQ(oldStats->edge_count, kind == EDGE ? owners : 0U);
            if (kind == EDGE) EXPECT_EQ(catalog.readTokenCardinality(born, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 7), std::optional<uint64_t>(birthOwner));
        }
        if (kind == NODE) { EXPECT_TRUE(metadata_->findNodePageLocationOptional(born, birthOwner)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(old, birthOwner)); }
        else { EXPECT_TRUE(metadata_->findEdgePageLocationOptional(born, birthOwner)); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(old, birthOwner)); }
        if (vector) {
            const auto manifest = vector_manifests_->read(born, propertyIndex).manifest();
            EXPECT_EQ(manifest.dimensions, 2U); EXPECT_EQ(manifest.entries.size(), 1U);
            EXPECT_EQ(VectorNodeMapping::findNode(born, *btree_pages_, *metadata_, manifest.vector_node_mapping_index_id, 1), std::optional<uint64_t>(birthOwner));
            EXPECT_TRUE(metadata_->findVectorLocationOptional(born, 1)); EXPECT_FALSE(metadata_->findVectorManifestOptional(old, propertyIndex));
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
            auto reopened = RelocatableMetadataStore::openReadOnly(io_); auto source = reopened->openSnapshot();
            EXPECT_TRUE(reopened->findVectorLocationOptional(source, 1)); EXPECT_TRUE(reopened->findVectorManifestOptional(source, propertyIndex)); return;
        }
        if (supportIndex) {
            const std::array<PropertyValue, 1> values{initial};
            const auto key = ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), birthOwner);
            EXPECT_EQ(BTreeIndex(supportIndex, *btree_pages_, *metadata_).find(born, key), std::optional<uint64_t>(birthOwner));
            EXPECT_FALSE(BTreeIndex(supportIndex, *btree_pages_, *metadata_).find(old, key));
        }
        const NativeHybridPropertyColumnReplacement cell{{kind, birthOwner}, 1, int64_t(birthOwner)};
        auto update = planner.beginTransaction(); update.stageHybridPropertyColumns(std::span(&cell, 1), memory, nullptr, true);
        ASSERT_TRUE(update.commit().committed()); const auto current = metadata_->openSnapshot();
        EXPECT_EQ(properties_->readEntityPropertyOptional(current, {kind, birthOwner}, 1)->value, PropertyValue(int64_t(birthOwner)));
        if (withIndex) {
            const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(birthOwner)), birthOwner);
            EXPECT_EQ(BTreeIndex(propertyIndex, *btree_pages_, *metadata_).find(current, key), std::optional<uint64_t>(birthOwner));
        }
        if (withProperties) EXPECT_EQ(properties_->readEntityPropertyOptional(born, {kind, birthOwner}, birthToken)->value, initial);
        else EXPECT_FALSE(properties_->readEntityPropertyOptional(born, {kind, birthOwner}, 1));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
        auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore values(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            PropertyCollectionRootStore roots(pages, values, *reopened); auto source = reopened->openSnapshot();
        EXPECT_EQ(roots.readEntityPropertyOptional(source, {kind, birthOwner}, 1)->value, PropertyValue(int64_t(birthOwner)));
    }
};
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PropertylessNodePublishesEntityAndLiveBitThenAcceptsItsFirstColumnField) {
    exercise({.kind = NODE});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PropertylessEdgePublishesEntityAdjacencyAndLiveBitThenAcceptsItsFirstColumnField) {
    exercise({.kind = EDGE});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialNodePublicationRecoversNeitherEntityNorLiveBit) {
    exercise({.kind = NODE, .completeFault = false});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteNodePublicationRecoversEntityAndLiveBitTogether) {
    exercise({.kind = NODE, .completeFault = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialEdgePublicationRecoversNeitherEntityNorLiveBit) {
    exercise({.kind = EDGE, .completeFault = false});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteEdgePublicationRecoversEntityAndLiveBitTogether) {
    exercise({.kind = EDGE, .completeFault = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PropertyBearingNodePublishesItsColumnAndEntityTogether) {
    exercise({.kind = NODE, .withProperties = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PropertyBearingEdgePublishesItsColumnAndEntityTogether) {
    exercise({.kind = EDGE, .withProperties = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNodeValueCanDemoteOnlyItsExistingField) {
    exercise({.kind = NODE, .withProperties = true, .initial = PropertyValue("new string")});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewEdgeValueCanDemoteOnlyItsExistingField) {
    exercise({.kind = EDGE, .withProperties = true, .initial = PropertyValue("new string")});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialNodeFieldPublicationRecoversNeitherEntityNorField) {
    exercise({.kind = NODE, .completeFault = false, .withProperties = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteNodeFieldPublicationRecoversEntityAndFieldTogether) {
    exercise({.kind = NODE, .completeFault = true, .withProperties = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialEdgeFieldPublicationRecoversNeitherEntityNorField) {
    exercise({.kind = EDGE, .completeFault = false, .withProperties = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteEdgeFieldPublicationRecoversEntityAndFieldTogether) {
    exercise({.kind = EDGE, .completeFault = true, .withProperties = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, SixteenNewNodesMergeTheirColumnBitsInOnePublication) {
    exercise({.kind = NODE, .withProperties = true, .owners = 48});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, SixteenNewEdgesMergeTheirColumnBitsAndAdjacencyTogether) {
    exercise({.kind = EDGE, .withProperties = true, .owners = 48});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNodeFieldWithoutAColumnUsesAnExtension) {
    exercise({.kind = NODE, .withProperties = true, .initial = PropertyValue("extension"), .extensionField = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewEdgeFieldWithoutAColumnUsesAnExtension) {
    exercise({.kind = EDGE, .withProperties = true, .initial = PropertyValue("extension"), .extensionField = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CatalogNodeCountAndNewNativeFieldPublishTogether) {
    exercise({.kind = NODE, .withProperties = true, .withCatalog = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CatalogEdgeTypeCountAndNativeFieldPublishTogether) {
    exercise({.kind = EDGE, .withProperties = true, .withCatalog = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNodeLargeFieldPublishesItsValueAndEntityTogether) {
    exercise({.kind = NODE, .withProperties = true, .initial = PropertyValue(std::string(70000, 'x')), .extensionField = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewEdgeLargeFieldPublishesItsValueAndEntityTogether) {
    exercise({.kind = EDGE, .withProperties = true, .initial = PropertyValue(std::string(70000, 'x')), .extensionField = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNodeLargeValueCanDemoteItsExistingColumn) {
    exercise({.kind = NODE, .withProperties = true, .initial = PropertyValue(std::string(70000, 'x'))});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewEdgeLargeValueCanDemoteItsExistingColumn) {
    exercise({.kind = EDGE, .withProperties = true, .initial = PropertyValue(std::string(70000, 'x'))});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNodeFieldNameAndValuePublishWithItsCatalogIdentity) {
    exercise({.kind = NODE, .withProperties = true, .initial = PropertyValue("new name"), .extensionField = true, .withCatalog = true, .allocateName = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewEdgeFieldNameAndValuePublishWithItsCatalogIdentity) {
    exercise({.kind = EDGE, .withProperties = true, .initial = PropertyValue("new name"), .extensionField = true, .withCatalog = true, .allocateName = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialNodeNamePublicationLeavesNoCatalogTokenOrEntity) {
    exercise({.kind = NODE, .completeFault = false, .withProperties = true, .initial = PropertyValue("new name"), .extensionField = true, .withCatalog = true, .allocateName = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteNodeNamePublicationRecoversCatalogTokenAndEntity) {
    exercise({.kind = NODE, .completeFault = true, .withProperties = true, .initial = PropertyValue("new name"), .extensionField = true, .withCatalog = true, .allocateName = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialEdgeNamePublicationLeavesNoCatalogTokenOrEntity) {
    exercise({.kind = EDGE, .completeFault = false, .withProperties = true, .initial = PropertyValue("new name"), .extensionField = true, .withCatalog = true, .allocateName = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteEdgeNamePublicationRecoversCatalogTokenAndEntity) {
    exercise({.kind = EDGE, .completeFault = true, .withProperties = true, .initial = PropertyValue("new name"), .extensionField = true, .withCatalog = true, .allocateName = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNativeNodeAndScalarIndexPublishTogether) {
    exercise({.kind = NODE, .withProperties = true, .withCatalog = true, .withIndex = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNativeEdgeAndScalarIndexPublishTogether) {
    exercise({.kind = EDGE, .withProperties = true, .withCatalog = true, .withIndex = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialNodeIndexPublicationRecoversAllOldRoots) {
    exercise({.kind = NODE, .completeFault = false, .withProperties = true, .withCatalog = true, .withIndex = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteNodeIndexPublicationRecoversAllNewRoots) {
    exercise({.kind = NODE, .completeFault = true, .withProperties = true, .withCatalog = true, .withIndex = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialEdgeIndexPublicationRecoversAllOldRoots) {
    exercise({.kind = EDGE, .completeFault = false, .withProperties = true, .withCatalog = true, .withIndex = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteEdgeIndexPublicationRecoversAllNewRoots) {
    exercise({.kind = EDGE, .completeFault = true, .withProperties = true, .withCatalog = true, .withIndex = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNativeNodeAndVectorBootstrapPublishTogether) {
    exercise({.kind = NODE, .withProperties = true, .initial = PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(100)), PropertyValue(int64_t(1))}), .withCatalog = true, .derived = DerivedCase::ODC_VECTOR});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialVectorBootstrapRecoversNoNodeFieldOrVector) {
    exercise({.kind = NODE, .completeFault = false, .withProperties = true, .initial = PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(100)), PropertyValue(int64_t(1))}), .withCatalog = true, .derived = DerivedCase::ODC_VECTOR});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteVectorBootstrapRecoversNodeFieldAndVector) {
    exercise({.kind = NODE, .completeFault = true, .withProperties = true, .initial = PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(100)), PropertyValue(int64_t(1))}), .withCatalog = true, .derived = DerivedCase::ODC_VECTOR});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNodePreservesUniqueSupportConstraint) {
    exercise({.kind = NODE, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_UNIQUE});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNodePreservesRequiredFieldConstraint) {
    exercise({.kind = NODE, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_REQUIRED});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewNodePreservesIntegerTypeConstraint) {
    exercise({.kind = NODE, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_INTEGER});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, MissingNodeRequiredFieldPublishesNothing) {
    exercise({.kind = NODE, .withCatalog = true, .derived = DerivedCase::ODC_REQUIRED, .expectFailure = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, InvalidNodePropertyTypePublishesNothing) {
    exercise({.kind = NODE, .withProperties = true, .initial = PropertyValue("invalid"), .withCatalog = true, .derived = DerivedCase::ODC_INTEGER, .expectFailure = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialNodeConstraintSupportPublicationRecoversTogether) {
    exercise({.kind = NODE, .completeFault = false, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_UNIQUE});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteNodeConstraintSupportPublicationRecoversTogether) {
    exercise({.kind = NODE, .completeFault = true, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_UNIQUE});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewEdgePreservesUniqueSupportConstraint) {
    exercise({.kind = EDGE, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_UNIQUE});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewEdgePreservesRequiredFieldConstraint) {
    exercise({.kind = EDGE, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_REQUIRED});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewEdgePreservesIntegerTypeConstraint) {
    exercise({.kind = EDGE, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_INTEGER});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, MissingEdgeRequiredFieldPublishesNothing) {
    exercise({.kind = EDGE, .withCatalog = true, .derived = DerivedCase::ODC_REQUIRED, .expectFailure = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, InvalidEdgePropertyTypePublishesNothing) {
    exercise({.kind = EDGE, .withProperties = true, .initial = PropertyValue("invalid"), .withCatalog = true, .derived = DerivedCase::ODC_INTEGER, .expectFailure = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialEdgeConstraintSupportPublicationRecoversTogether) {
    exercise({.kind = EDGE, .completeFault = false, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_UNIQUE});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteEdgeConstraintSupportPublicationRecoversTogether) {
    exercise({.kind = EDGE, .completeFault = true, .withProperties = true, .withCatalog = true, .derived = DerivedCase::ODC_UNIQUE});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, TwoNewNodesCannotPublishTheSameUniqueValue) {
    exercise({.kind = NODE, .withProperties = true, .owners = 62, .withCatalog = true, .derived = DerivedCase::ODC_UNIQUE, .expectFailure = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, NewEdgeCannotDuplicateAnExistingNativeUniqueValue) {
    exercise({.kind = EDGE, .withProperties = true, .initial = PropertyValue(int64_t(1)), .withCatalog = true, .derived = DerivedCase::ODC_UNIQUE, .expectFailure = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, RequiredFieldFailureDoesNotPublishANewCatalogName) {
    exercise({.kind = NODE, .withProperties = true, .initial = PropertyValue("new name"), .extensionField = true, .withCatalog = true, .allocateName = true, .derived = DerivedCase::ODC_REQUIRED, .expectFailure = true});
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointNodeBirthAndExistingFieldChangeShareTheirColumnImage) {
    exercise({.kind = NODE, .withProperties = true, .updateExisting = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointEdgeBirthAndExistingFieldChangeShareTheirColumnImage) {
    exercise({.kind = EDGE, .withProperties = true, .updateExisting = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointNodeBirthAndExistingFieldDemotionShareTheirGroupLiveMask) {
    exercise({.kind = NODE, .withProperties = true, .updateExisting = true, .existingValue = PropertyValue("changed")});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialJointNodeBirthAndExistingFieldPublicationKeepsBothOldStates) {
    exercise({.kind = NODE, .completeFault = false, .withProperties = true, .updateExisting = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteJointNodeBirthAndExistingFieldPublicationRecoversBothNewStates) {
    exercise({.kind = NODE, .completeFault = true, .withProperties = true, .updateExisting = true});
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointNodeAndEdgeBirthPublishBothNativeGroupLiveMasks) {
    exercise({.kind = NODE, .withProperties = true, .bothKinds = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointPropertylessNodeAndEdgeBirthPreserveBothNativeColumns) {
    exercise({.kind = NODE, .bothKinds = true});
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointNodeBirthAndDeletionMergeOneFinalColumnAndLiveMask) {
    exercise({.kind = NODE, .withProperties = true, .deleteExisting = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointEdgeBirthAndDeletionMergeTheirColumnAndCanonicalAdjacency) {
    exercise({.kind = EDGE, .withProperties = true, .deleteExisting = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointPropertylessBirthAndDeletionDemoteOnlyTheRemainingSourceMembers) {
    exercise({.kind = NODE, .owners = 48, .deleteExisting = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, PartialJointBirthAndDeletionRetainTheOldOwnerAndAllOldFields) {
    exercise({.kind = NODE, .completeFault = false, .withProperties = true, .deleteExisting = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, CompleteJointBirthAndDeletionPublishTheNewOwnerAndAllNewFields) {
    exercise({.kind = NODE, .completeFault = true, .withProperties = true, .deleteExisting = true});
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointSecondGroupBirthAndFirstGroupFieldChangePreserveBothSources) {
    exercise({.kind = NODE, .withProperties = true, .owners = 127, .updateExisting = true});
}
TEST_F(HybridPropertyOwnerInsertionPublicationTest, JointSecondGroupBirthAndFirstGroupDemotionKeepTheirLiveMasksSeparate) {
    exercise({.kind = NODE, .withProperties = true, .owners = 127, .updateExisting = true, .existingValue = PropertyValue("changed")});
}
#endif
