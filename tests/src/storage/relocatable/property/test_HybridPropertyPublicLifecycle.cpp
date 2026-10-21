#include "storage/relocatable/property/HybridPropertyQueryParityTestFixture.hpp"
#include "graph/storage/PwriteHelper.hpp"

using namespace hybrid_property_query_parity_test;

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyQueryParityTest, RowCreationAfterNativeFieldWriteKeepsPoolNamespacesSeparate) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(100))}, {"note", PropertyValue("updated")}}}});
    auto old = backend.openLogicalGraphReadSnapshot();
    const auto ids = backend.createLogicalNodes({{{"Person"}, {{"age", PropertyValue(int64_t(75))}, {"note", PropertyValue("row")}}}});
    ASSERT_EQ(ids.size(), 1U); ASSERT_GT(ids.front(), 64U);
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(ids.front(), "age"), PropertyValue(int64_t(75)));
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(100)));
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(100)));
    current.reset(); old.reset(); backend.close();
    backend.open(); current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(ids.front(), "note"), PropertyValue("row"));
    EXPECT_EQ(current->readNodeProperty(1, "note"), PropertyValue("updated"));
    current.reset(); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, OrdinaryPlacementNeverGrantsReadOrAllocationAuthorityForNativePools) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::openReadOnly(io); auto snapshot = metadata->openSnapshot();
    SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    HybridPropertyReadContext context(io, pages, *metadata, snapshot);
    SlottedSlotReference reference; ASSERT_TRUE(context.groupRecord({EntityPropertyOwnerKind::EPOK_NODE, 1}, {}, &reference));
    const auto location = metadata->findPageLocation(snapshot, reference.page_id);
    const std::array pools{location.segment_id}; const auto epoch = io->dataMutationEpoch();
    EXPECT_FALSE(pages.findBestFitPage(location.segment_id, 1U));
    EXPECT_FALSE(pages.findBestFitPageForRecord(snapshot, pools, PageOwnerKind::POK_PROPERTY, 1U));
    EXPECT_FALSE(pages.findBestFitPageForRecord(snapshot, pools, PageOwnerKind::POK_HYBRID_PROPERTY, 1U));
    EXPECT_THROW((void)pages.read(snapshot, reference.page_id), FormatException);
    SharedPagePoolAllocator ordinary(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    EXPECT_THROW((void)ordinary.read(snapshot, reference.page_id), FormatException);
    EXPECT_EQ(io->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyQueryParityTest, JointPropertyPublicationCoordinatesNativeAndRowLargeValuesAndNewNames) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalNodes({{{"Person"}, {{"age", PropertyValue(int64_t(75))}}}});
    ASSERT_EQ(ids.size(), 1U); auto old = backend.openLogicalGraphReadSnapshot();
    const std::string native(70000, 'n'), row(70000, 'r');
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(100))}, {"joint_native", PropertyValue(native)}}},
        {ids.front(), {{"age", PropertyValue(int64_t(101))}, {"joint_row", PropertyValue(row)}}}});
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "joint_native"), PropertyValue(native));
    EXPECT_EQ(current->readNodeProperty(ids.front(), "joint_row"), PropertyValue(row));
    EXPECT_EQ(old->readNodeProperty(1, "joint_native"), PropertyValue());
    EXPECT_EQ(old->readNodeProperty(ids.front(), "joint_row"), PropertyValue());
    current.reset(); old.reset(); backend.close(); inspectPublicNativeState();
    backend.open(); current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "joint_native"), PropertyValue(native));
    EXPECT_EQ(current->readNodeProperty(ids.front(), "joint_row"), PropertyValue(row));
    current.reset(); backend.close();
}
TEST_F(HybridPropertyQueryParityTest, JointPropertyPublicationRelabelsNativeOwnerAndMaintainsItsFieldAndScalarIndex) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    LogicalGraphMutationBatch batch;
    const LogicalGraphMutationEntityReference owner{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1};
    batch.node_label_replacements.push_back({owner, {"Solo"}});
    batch.property_replacements.push_back({owner, {{"age", PropertyValue(int64_t(100))}, {"note", PropertyValue("relabelled")}}});
    (void)backend.executeLogicalGraphMutationBatch(batch);
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(100)));
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    current.reset(); old.reset(); batch.node_label_replacements.front().labels = {"Person"};
    batch.property_replacements.front().properties = {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")}};
    (void)backend.executeLogicalGraphMutationBatch(batch); backend.close();
    EXPECT_EQ(read(), baseline); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, JointPropertyPublicationMergesNativeAndRowPatchesWithoutDroppingOtherFields) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalNodes({{{"Person"}, {{"age", PropertyValue(int64_t(75))}, {"note", PropertyValue("row")}}}});
    ASSERT_EQ(ids.size(), 1U); auto old = backend.openLogicalGraphReadSnapshot(); const auto fence = old->mutationFence(); ASSERT_TRUE(fence);
    const std::array<LogicalGraphPropertyPatchRequest, 2> patches{{
        {{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {{"joint_patch", PropertyValue("native")}}, {"note"}},
        {{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, ids.front()}, {{"joint_patch", PropertyValue("row")}}, {"note"}}}};
    (void)backend.executeLogicalGraphMutationBatchWithPropertyPatches(*old, {}, patches, 4096, *fence);
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    EXPECT_EQ(current->readNodeProperty(ids.front(), "age"), PropertyValue(int64_t(75)));
    EXPECT_EQ(current->readNodeProperty(1, "joint_patch"), PropertyValue("native"));
    EXPECT_EQ(current->readNodeProperty(ids.front(), "joint_patch"), PropertyValue("row"));
    EXPECT_EQ(current->readNodeProperty(1, "note"), PropertyValue());
    EXPECT_EQ(current->readNodeProperty(ids.front(), "note"), PropertyValue());
    EXPECT_EQ(old->readNodeProperty(1, "note"), PropertyValue("person"));
    EXPECT_EQ(old->readNodeProperty(ids.front(), "note"), PropertyValue("row"));
    current.reset(); old.reset(); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, JointPropertyPublicationRecoversBothLayoutsLargeValuesAndVectorTogether) {
    seed(false, false, false, false, 64, false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    uint64_t rowId;
    { RelocatableStorageBackend backend(options); backend.open(); const auto ids = backend.createLogicalNodes({{{"Other"}, {{"age", PropertyValue(int64_t(75))}}}});
      ASSERT_EQ(ids.size(), 1U); rowId = ids.front(); backend.close(); }
    const std::string native(70000, 'n'), row(70000, 'r');
    const PropertyValue vector(std::vector<PropertyValue>{PropertyValue(int64_t(200)), PropertyValue(int64_t(1))});
    for (const bool complete : {false, true}) {
        RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot(); bool injected = false;
        {
            StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) -> pwrite_ssize_t {
                if (!injected && offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES && count == RSS_SUPERBLOCK_BYTES) {
                    injected = true;
                    if (!complete) return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_BYTES / 2U, offset);
                    (void)portable_pwrite(fd, bytes, count, offset); throw std::runtime_error("joint property header interruption");
                }
                return portable_pwrite(fd, bytes, count, offset);
            });
            EXPECT_THROW(backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(0))}, {"embedding", vector}, {"joint_fault", PropertyValue(native)}}},
                {rowId, {{"age", PropertyValue(int64_t(75))}, {"joint_fault", PropertyValue(row)}}}}), std::runtime_error);
        }
        ASSERT_TRUE(injected); EXPECT_EQ(old->readNodeProperty(1, "joint_fault"), PropertyValue());
        EXPECT_EQ(old->readNodeProperty(rowId, "joint_fault"), PropertyValue()); old.reset(); backend.close();
        options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
        { RelocatableStorageBackend reopened(options); reopened.open(); auto current = reopened.openLogicalGraphReadSnapshot();
          EXPECT_EQ(current->readNodeProperty(1, "joint_fault"), complete ? PropertyValue(native) : PropertyValue());
          EXPECT_EQ(current->readNodeProperty(rowId, "joint_fault"), complete ? PropertyValue(row) : PropertyValue());
          current.reset(); reopened.close(); }
        const auto hits = exactVectorSearch(); ASSERT_FALSE(hits.empty()); EXPECT_EQ(hits.front().first, complete ? 2U : 1U);
        inspectPublicNativeState(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    }
}
TEST_F(HybridPropertyQueryParityTest, JointOwnerDeletionAndFieldChangeMergeTheSameNativeColumn) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    LogicalGraphMutationBatch batch; batch.node_deletes.push_back({64, false, 0});
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {{"age", PropertyValue(int64_t(100))}, {"note", PropertyValue("survivor")}}});
    (void)backend.executeLogicalGraphMutationBatch(batch); auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(100)));
    EXPECT_EQ(current->readNodeProperty(1, "note"), PropertyValue("survivor"));
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    EXPECT_EQ(old->readNodeProperty(64, "age"), PropertyValue(int64_t(63)));
    current.reset(); old.reset(); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, JointOwnerDeletionAndFieldChangeShareThresholdDemotionAndLargeExtension) {
    seed(false, false, false, false, 64, false, false, false, 48); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    const std::string large(70000, 's'); LogicalGraphMutationBatch batch; batch.node_deletes.push_back({48, false, 0});
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {{"age", PropertyValue(int64_t(100))}, {"joint_survivor", PropertyValue(large)}}});
    (void)backend.executeLogicalGraphMutationBatch(batch); auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(100)));
    EXPECT_EQ(current->readNodeProperty(1, "joint_survivor"), PropertyValue(large));
    EXPECT_EQ(current->readNodeProperty(47, "age"), PropertyValue(int64_t(46)));
    EXPECT_EQ(old->readNodeProperty(48, "age"), PropertyValue(int64_t(47)));
    current.reset(); old.reset(); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NativeSchemaLifecycleCreatesAndDropsScalarIndexWithoutChangingFields) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    ASSERT_NO_THROW((void)backend.createLogicalNodeIndex({"native_person_age", "Person", {"age"}}));
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(100))}, {"note", PropertyValue("person")}}}});
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); old.reset();
    EXPECT_TRUE(backend.dropLogicalIndexByName("native_person_age"));
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")}}}});
    backend.close(); EXPECT_EQ(read(), baseline); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NativeSchemaLifecycleCreatesRequiredAndUniqueConstraintsAndMaintainsTheirSupport) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    (void)backend.createLogicalConstraint({"native_required", LogicalGraphConstraintEntityKind::LGCEK_NODE,
        LogicalGraphConstraintKind::LGCK_NOT_NULL, "Person", {"age"}, LogicalGraphConstraintPropertyType::LGCPT_NONE});
    (void)backend.createLogicalConstraint({"native_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
        LogicalGraphConstraintKind::LGCK_UNIQUE, "Person", {"age"}, LogicalGraphConstraintPropertyType::LGCPT_NONE});
    EXPECT_THROW(backend.replaceLogicalNodeProperties({{1, {{"note", PropertyValue("missing")}}}}), std::runtime_error);
    EXPECT_THROW(backend.replaceLogicalNodeProperties({{2, {{"age", PropertyValue(int64_t(0))}}}}), std::runtime_error);
    backend.replaceLogicalNodeProperties({{2, {{"age", PropertyValue(int64_t(100))}, {"note", PropertyValue("person")}}}});
    auto old = backend.openLogicalGraphReadSnapshot(); EXPECT_TRUE(backend.dropLogicalConstraintByName("native_unique"));
    EXPECT_TRUE(backend.dropLogicalConstraintByName("native_required"));
    backend.replaceLogicalNodeProperties({{1, {{"note", PropertyValue("removed")}}}});
    backend.replaceLogicalNodeProperties({{2, {{"age", PropertyValue(int64_t(0))}}}});
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    EXPECT_EQ(old->readNodeProperty(2, "age"), PropertyValue(int64_t(100)));
    old.reset(); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NativeSchemaLifecycleRebuildsVectorDefinitionAndThenCoordinatesNativeUpdates) {
    seed(false, false, false, false, 64, false, false, true);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    { RelocatableStorageBackend backend(options); backend.open(); ASSERT_TRUE(backend.dropLogicalIndexByName("person_embedding")); backend.close(); }
    convert(EntityPropertyOwnerKind::EPOK_NODE);
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    (void)backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2, "l2"});
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(0))}, {"embedding",
        PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(200)), PropertyValue(int64_t(1))})}}}});
    EXPECT_EQ(old->readNodeProperty(1, "embedding"), PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(0)), PropertyValue(int64_t(1))}));
    old.reset(); backend.close(); const auto hits = exactVectorSearch(); ASSERT_FALSE(hits.empty()); EXPECT_EQ(hits.front().first, 2U);
    backend.open(); ASSERT_TRUE(backend.dropLogicalIndexByName("person_embedding")); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NativeSchemaLifecycleRecoversCatalogDefinitionAndScalarRootAcrossBothHeaders) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    for (const bool complete : {false, true}) {
        RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot(); bool injected = false;
        {
            StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) -> pwrite_ssize_t {
                if (!injected && offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES && count == RSS_SUPERBLOCK_BYTES) {
                    injected = true;
                    if (!complete) return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_BYTES / 2U, offset);
                    (void)portable_pwrite(fd, bytes, count, offset); throw std::runtime_error("native schema header interruption");
                }
                return portable_pwrite(fd, bytes, count, offset);
            });
            EXPECT_THROW((void)backend.createLogicalNodeIndex({"native_fault_index", "Person", {"age"}}), std::runtime_error);
        }
        ASSERT_TRUE(injected); EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); old.reset(); backend.close();
        auto readOnly = options; readOnly.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
        RelocatableStorageBackend reopened(readOnly); reopened.open(); const auto indexes = reopened.listLogicalIndexes(64);
        EXPECT_EQ(std::ranges::any_of(indexes, [](const auto &index) { return index.name == "native_fault_index"; }), complete);
        reopened.close(); inspectPublicNativeState();
    }
}
TEST_F(HybridPropertyQueryParityTest, NativeSchemaLifecycleCreatesCompositeIndexAndKeepsItsLogicalFieldOrder) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    (void)backend.createLogicalNodeIndex({"native_composite", "Person", {"note", "age"}});
    auto current = backend.openLogicalGraphReadSnapshot();
    const std::vector<std::pair<std::string, PropertyValue>> keys{{"note", PropertyValue("person")}, {"age", PropertyValue(int64_t(1))}};
    const auto found = current->findNodesByExactProperties({"Person"}, keys, 64, 64);
    ASSERT_TRUE(found); ASSERT_EQ(found->size(), 1U); EXPECT_EQ(found->front().node_id, 2U); current.reset();
    backend.replaceLogicalNodeProperties({{2, {{"age", PropertyValue(int64_t(100))}, {"note", PropertyValue("changed")}}}});
    EXPECT_EQ(old->readNodeProperty(2, "age"), PropertyValue(int64_t(1))); old.reset();
    ASSERT_TRUE(backend.dropLogicalIndexByName("native_composite")); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NativeSchemaLifecycleRejectsInvalidUniqueDefinitionWithoutPublishingIt) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot(); const auto fence = old->mutationFence();
    EXPECT_THROW((void)backend.createLogicalConstraint({"native_duplicate_note", LogicalGraphConstraintEntityKind::LGCEK_NODE,
        LogicalGraphConstraintKind::LGCK_UNIQUE, "Person", {"note"}, LogicalGraphConstraintPropertyType::LGCPT_NONE}), std::runtime_error);
    EXPECT_TRUE(backend.listLogicalConstraints(64).empty());
    auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(current->mutationFence(), fence);
    current.reset(); old.reset(); backend.close(); EXPECT_EQ(read(), baseline); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NativeSchemaLifecycleBuildsEdgeConstraintAndMaintainsItsNativeSupportEntries) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    (void)backend.createLogicalConstraint({"native_edge_weight", LogicalGraphConstraintEntityKind::LGCEK_EDGE,
        LogicalGraphConstraintKind::LGCK_UNIQUE, "KNOWS", {"weight"}, LogicalGraphConstraintPropertyType::LGCPT_NONE});
    EXPECT_THROW(backend.replaceLogicalEdgeProperties({{2, {{"weight", PropertyValue(int64_t(0))}}}}), std::runtime_error);
    backend.replaceLogicalEdgeProperties({{2, {{"weight", PropertyValue(int64_t(100))}}}});
    ASSERT_TRUE(backend.dropLogicalConstraintByName("native_edge_weight"));
    backend.replaceLogicalEdgeProperties({{2, {{"weight", PropertyValue(int64_t(0))}}}});
    backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, OrdinaryTopologyCreatesAndDeletesRowOwnersOutsideNativeGroups) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); ASSERT_NO_THROW(backend.open()); auto old = backend.openLogicalGraphReadSnapshot();
    const auto nodes = backend.createLogicalNodes({{{"Person"}, {{"age", PropertyValue(int64_t(75))}}}});
    ASSERT_EQ(nodes.size(), 1U); ASSERT_GT(nodes.front(), 64U);
    const auto edges = backend.createLogicalEdges({{1, nodes.front(), "KNOWS", {{"weight", PropertyValue(int64_t(1000))}}}});
    ASSERT_EQ(edges.size(), 1U);
    auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(current->readNodeProperty(nodes.front(), "age"), PropertyValue(int64_t(75)));
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); current.reset();
    backend.deleteLogicalEdges({{edges.front()}}); backend.deleteLogicalNodes({{nodes.front(), false, 0}});
    old.reset(); backend.close(); EXPECT_EQ(read(), baseline);
}
TEST_F(HybridPropertyQueryParityTest, OrdinaryTopologyRelabelPreservesNativeValuesAndMaintainsScalarMembership) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); ASSERT_NO_THROW(backend.open()); auto old = backend.openLogicalGraphReadSnapshot();
    LogicalGraphMutationBatch batch;
    batch.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {"Solo"}});
    (void)backend.executeLogicalGraphMutationBatch(batch);
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    current.reset(); batch.node_label_replacements.front().labels = {"Person"}; (void)backend.executeLogicalGraphMutationBatch(batch);
    old.reset(); backend.close(); EXPECT_EQ(read(), baseline);
}
TEST_F(HybridPropertyQueryParityTest, OrdinaryTopologyRelabelCoordinatesVectorScopeWithoutChangingItsNativeField) {
    seed(false, false, false, false, 64, false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); ASSERT_NO_THROW(backend.open());
    LogicalGraphMutationBatch batch;
    batch.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {"Solo"}});
    (void)backend.executeLogicalGraphMutationBatch(batch); backend.close();
    auto result = exactVectorSearch(); ASSERT_FALSE(result.empty()); EXPECT_EQ(result.front().first, 2U);
    backend.open(); auto snapshot = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(snapshot->readNodeProperty(1, "embedding"), PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(0)), PropertyValue(int64_t(1))}));
    snapshot.reset(); batch.node_label_replacements.front().labels = {"Person"}; (void)backend.executeLogicalGraphMutationBatch(batch); backend.close();
    result = exactVectorSearch(); ASSERT_FALSE(result.empty()); EXPECT_EQ(result.front().first, 1U);
}
TEST_F(HybridPropertyQueryParityTest, OrdinaryTopologyReopenSkipsUnusedNativeGroupIdsWithoutChangingItsLiveness) {
    seed(false, false, false, false, 64, false, false, false, 48); convert(EntityPropertyOwnerKind::EPOK_NODE);
    const auto before = read(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalNodes({{{"Person"}, {{"age", PropertyValue(int64_t(75))}}}});
    ASSERT_EQ(ids.size(), 1U); EXPECT_GT(ids.front(), 64U);
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(snapshot->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    EXPECT_EQ(snapshot->readNodeProperty(ids.front(), "age"), PropertyValue(int64_t(75)));
    snapshot.reset(); backend.deleteLogicalNodes({{ids.front(), false, 0}});
    backend.close(); EXPECT_EQ(read(), before);
}
TEST_F(HybridPropertyQueryParityTest, PublicNativePropertyReplacementMaintainsScalarIndexAndPinnedValues) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(100))}, {"note", PropertyValue("updated")}}}});
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(100)));
    EXPECT_EQ(current->readNodeProperty(1, "note"), PropertyValue("updated"));
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    current.reset(); old.reset(); backend.close();
    EXPECT_NE(read(), baseline); backend.open();
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")}}}});
    backend.close(); EXPECT_EQ(read(), baseline); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, PublicNativePropertyReplacementCoordinatesVectorAndPreservesOldSnapshot) {
    seed(false, false, false, false, 64, false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    const PropertyValue previous(std::vector<PropertyValue>{PropertyValue(int64_t(0)), PropertyValue(int64_t(1))});
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")},
        {"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1000)), PropertyValue(int64_t(1))})}}}});
    EXPECT_EQ(old->readNodeProperty(1, "embedding"), previous); old.reset(); backend.close();
    auto result = exactVectorSearch(); ASSERT_FALSE(result.empty()); EXPECT_EQ(result.front().first, 2U);
    backend.open(); backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(0))},
        {"note", PropertyValue("person")}, {"embedding", previous}}}}); backend.close();
    result = exactVectorSearch(); ASSERT_FALSE(result.empty()); EXPECT_EQ(result.front().first, 1U); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, PublicNativePropertyReplacementEnforcesRequiredFieldsAndPublishesNewTokensAtomically) {
    seed(true, true); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    try {
        backend.replaceLogicalNodeProperties({{1, {{"note", PropertyValue("missing age")}}}});
        FAIL() << "required native field was removed";
    } catch (const std::runtime_error &error) {
        EXPECT_NE(std::string(error.what()).find("Constraint violation"), std::string::npos);
    }
    auto old = backend.openLogicalGraphReadSnapshot();
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")}, {"new_field", PropertyValue(int64_t(1))}}}});
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "new_field"), PropertyValue(int64_t(1)));
    EXPECT_EQ(old->readNodeProperty(1, "new_field"), PropertyValue());
    old.reset(); current.reset(); backend.close(); EXPECT_EQ(read(), baseline); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, PublicNativePropertyReplacementAccepts64OwnersAndPreservesNoopGeneration) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    const auto generation = [&] {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::openReadOnly(io); return metadata->openSnapshot().superblock().generation;
    };
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    std::vector<LogicalGraphPropertyReplaceRequest> replacements;
    for (uint64_t id = 1; id <= 64; ++id)
        replacements.push_back({id, {{"age", PropertyValue(int64_t(id))}, {"note", PropertyValue(id == 8 ? "seven" : "person")}}});
    backend.replaceLogicalNodeProperties(replacements); const auto after = generation();
    backend.replaceLogicalNodeProperties(replacements); EXPECT_EQ(generation(), after);
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(snapshot->readNodeProperty(1, "age"), PropertyValue(int64_t(1)));
    EXPECT_EQ(snapshot->readNodeProperty(64, "age"), PropertyValue(int64_t(64)));
    snapshot.reset(); backend.close();
}
TEST_F(HybridPropertyQueryParityTest, PublicNativePropertyReplacementCoordinatesMixedRoutesAndPinnedValues) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalNodes({{{"Person"}, {{"age", PropertyValue(int64_t(75))}}}});
    ASSERT_EQ(ids.size(), 1U); ASSERT_GT(ids.front(), 64U);
    auto old = backend.openLogicalGraphReadSnapshot();
    ASSERT_NO_THROW(backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(100))}}},
        {ids.front(), {{"age", PropertyValue(int64_t(101))}}}}));
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(snapshot->readNodeProperty(1, "age"), PropertyValue(int64_t(100)));
    EXPECT_EQ(snapshot->readNodeProperty(ids.front(), "age"), PropertyValue(int64_t(101)));
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    EXPECT_EQ(old->readNodeProperty(ids.front(), "age"), PropertyValue(int64_t(75)));
    snapshot.reset(); old.reset(); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NativeWritableReopenReclaimsOrphanQueryScratchAndPreservesFields) {
    seed(false); const auto scratch = makeOrphanScratch(SegmentKind::SGK_QUERY_SCRATCH).segment_id;
    convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); ASSERT_NO_THROW(backend.open()); backend.close();
    EXPECT_EQ(read(), baseline); inspectPublicNativeState();
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::openReadOnly(io); auto pin = metadata->openSnapshot();
    EXPECT_EQ(metadata->findCatalog(pin, scratch).state, SegmentState::SGS_OBSOLETE);
}
TEST_F(HybridPropertyQueryParityTest, NativeWritableReopenReclaimsOrphanBulkScratchAndPreservesFields) {
    seed(false); const auto scratch = makeOrphanScratch(SegmentKind::SGK_BULK_SCRATCH);
    convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); ASSERT_NO_THROW(backend.open()); backend.close();
    EXPECT_EQ(read(), baseline); inspectPublicNativeState();
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::openReadOnly(io); auto pin = metadata->openSnapshot(); bool found = false;
    metadata->visitCatalogEntries(pin, [&](const auto &entry) { found |= entry.segment_id == scratch.segment_id; }); EXPECT_FALSE(found);
}
TEST_F(HybridPropertyQueryParityTest, NativeBulkScratchRetirementWaitsForOldPinThenRecoversWithoutChangingFields) {
    seed(false); const auto scratch = makeOrphanScratch(SegmentKind::SGK_BULK_SCRATCH); convert(EntityPropertyOwnerKind::EPOK_NODE);
    {
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io); PhysicalSegmentAllocator physical(io, *metadata);
        NativeBulkRunStore store(physical, *metadata, {PhysicalSlotId(40), PhysicalSlotId(180)});
        auto old = metadata->openSnapshot(); const auto slot = metadata->findCatalog(old, scratch.segment_id).physical_slot;
        store.release(scratch, LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1));
        auto current = metadata->openSnapshot(); EXPECT_EQ(metadata->findCatalog(current, scratch.segment_id).state, SegmentState::SGS_RETIRED);
        EXPECT_TRUE(metadata->isPhysicalSlotAllocated(current, slot));
        EXPECT_EQ(store.readOpaquePayload(old, scratch), std::vector<std::byte>{std::byte{0x11}});
        const auto lsn = LogSequenceNumber(current.superblock().checkpoint_lsn.value() + 1); current.release(); old.release();
        store.reclaimUnpublishedOnOpen(lsn); auto reclaimed = metadata->openSnapshot(); EXPECT_FALSE(metadata->isPhysicalSlotAllocated(reclaimed, slot)); io->flushStream();
    }
    inspectPublicNativeState(); EXPECT_EQ(read()[0].size(), 5U);
}
TEST_F(HybridPropertyQueryParityTest, NativeActiveLayoutAllowsFreshPrivateScratchWithoutChangingGraphValues) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    for (const auto kind : {SegmentKind::SGK_QUERY_SCRATCH, SegmentKind::SGK_BULK_SCRATCH}) {
        const auto scratch = makeOrphanScratch(kind); EXPECT_TRUE(scratch.isValid());
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); ASSERT_NO_THROW(backend.open()); backend.close();
        EXPECT_EQ(read(), baseline); inspectPublicNativeState();
    }
}
TEST_F(HybridPropertyQueryParityTest, NativeSegmentNamespaceReservationPreservesFieldsAcrossBothBootstrapFaults) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        uint64_t beforeId;
        {
            auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
            auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
            auto metadata = RelocatableMetadataStore::open(io); const auto before = metadata->openSnapshot().superblock(); beforeId = before.next_logical_segment_id;
            MetadataPublishOptions options; options.bootstrap_failpoint = point;
            EXPECT_THROW((void)metadata->allocateLogicalSegmentIds(1, LogSequenceNumber(before.checkpoint_lsn.value() + 1), options), std::runtime_error); io->flushStream();
        }
        {
            auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
            auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
            auto metadata = RelocatableMetadataStore::openReadOnly(io);
            EXPECT_EQ(metadata->openSnapshot().superblock().next_logical_segment_id,
                beforeId + (point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE ? 1U : 0U));
        }
        EXPECT_EQ(read(), baseline); inspectPublicNativeState();
    }
}
TEST_F(HybridPropertyQueryParityTest, PublicNativePatchUpdatesScalarAndRemovesFieldWithoutChangingOldSnapshot) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    applyPublicNativePatch(backend, {{"age", PropertyValue(int64_t(100))}}, {"note"});
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(100)));
    EXPECT_EQ(current->readNodeProperty(1, "note"), PropertyValue());
    EXPECT_EQ(old->readNodeProperty(1, "note"), PropertyValue("person")); old.reset(); current.reset();
    applyPublicNativePatch(backend, {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")}});
    backend.close(); EXPECT_EQ(read(), baseline); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, PublicNativePatchErasesAndRecreatesVectorFieldAtomically) {
    seed(false, false, false, false, 64, false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    applyPublicNativePatch(backend, {}, {"embedding"}); backend.close();
    auto results = exactVectorSearch(); ASSERT_FALSE(results.empty()); EXPECT_EQ(results.front().first, 2U);
    backend.open(); applyPublicNativePatch(backend, {{"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(0)), PropertyValue(int64_t(1))})}});
    backend.close(); results = exactVectorSearch(); ASSERT_FALSE(results.empty()); EXPECT_EQ(results.front().first, 1U); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, PublicNativePatchPreservesUnknownRemovalNoopAndRejectsRequiredFieldRemoval) {
    seed(true, true); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    const auto fence = old->mutationFence(); applyPublicNativePatch(backend, {}, {"unknown_field"});
    auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(current->mutationFence(), fence); current.reset(); old.reset();
    try { applyPublicNativePatch(backend, {}, {"age"}); FAIL() << "required field was removed"; }
    catch (const std::runtime_error &error) { EXPECT_NE(std::string(error.what()).find("Constraint violation"), std::string::npos); }
    backend.close(); EXPECT_EQ(read(), baseline); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, PublicNativeOwnerDeletionRemovesIsolatedNodeAndKeepsOldPins) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    backend.deleteLogicalNodes({{64, false, 0}});
    EXPECT_EQ(old->readNodeProperty(64, "age"), PropertyValue(int64_t(63)));
    auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    old.reset(); current.reset(); backend.close(); inspectPublicNativeState();
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::openReadOnly(io); auto pin = metadata->openSnapshot();
    EXPECT_FALSE(metadata->findNodePageLocationOptional(pin, 64));
}
TEST_F(HybridPropertyQueryParityTest, PublicNativeOwnerDeletionMaintainsConvertedEdgeAdjacencyAndCounts) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    backend.deleteLogicalEdges({{1}}); EXPECT_EQ(old->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0)));
    old.reset(); backend.close(); inspectPublicNativeState();
    const auto rows = read()[6]; ASSERT_EQ(rows.size(), 63U); EXPECT_EQ(rows.front()[0], PropertyValue(int64_t(1)));
}
TEST_F(HybridPropertyQueryParityTest, PublicNativeOwnerDeletionRemovesVectorMembershipAndPreservesOldEmbedding) {
    seed(false, false, false, false, 64, false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    backend.deleteLogicalNodes({{64, false, 0}});
    EXPECT_EQ(old->readNodeProperty(64, "embedding"), PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(63)), PropertyValue(int64_t(1))}));
    old.reset(); backend.close(); inspectPublicNativeState();
    const auto results = exactVectorSearch({63.0F, 1.0F}); ASSERT_FALSE(results.empty()); EXPECT_EQ(results.front().first, 63U);
    for (const auto &result : results) EXPECT_NE(result.first, 64U);
}
TEST_F(HybridPropertyQueryParityTest, PublicNativeOwnerDeletionRemovesComplete64EdgeGroupAndPreservesPinnedRows) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    std::vector<LogicalGraphEdgeDeleteRequest> requests;
    for (uint64_t id = 1; id <= 64; ++id) requests.push_back({id});
    backend.deleteLogicalEdges(requests);
    EXPECT_EQ(old->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0)));
    EXPECT_EQ(old->readEdgeProperty(64, "weight"), PropertyValue(int64_t(63)));
    old.reset(); backend.close(); inspectPublicNativeState(); EXPECT_TRUE(read()[6].empty());
}
TEST_F(HybridPropertyQueryParityTest, JointOwnerDeletionRejectsIncidentNodeAndThenDetachesItsRowEdges) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    EXPECT_THROW(backend.deleteLogicalNodes({{1, false, 0}}), FormatException);
    backend.deleteLogicalNodes({{1, true, 64}});
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
    EXPECT_EQ(old->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0)));
    old.reset(); backend.close(); EXPECT_EQ(read()[6].size(), 62U); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, JointOwnerDeletionRemovesNativeAndRowNodesWhileOldPinsRemainLive) {
    seed(); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalNodes({{{"Person"}, {{"age", PropertyValue(int64_t(75))}}}});
    ASSERT_EQ(ids.size(), 1U); auto old = backend.openLogicalGraphReadSnapshot();
    backend.deleteLogicalNodes({{64, false, 0}, {ids.front(), false, 0}});
    EXPECT_EQ(old->readNodeProperty(64, "age"), PropertyValue(int64_t(63)));
    EXPECT_EQ(old->readNodeProperty(ids.front(), "age"), PropertyValue(int64_t(75)));
    old.reset(); backend.close(); inspectPublicNativeState();
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::openReadOnly(io); const auto pin = metadata->openSnapshot();
    EXPECT_FALSE(metadata->findNodePageLocationOptional(pin, 64));
    EXPECT_FALSE(metadata->findNodePageLocationOptional(pin, ids.front()));
}
TEST_F(HybridPropertyQueryParityTest, JointOwnerDeletionRemovesNativeAndRowEdgesAndCanonicalAdjacencyTogether) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalEdges({{1, 2, "KNOWS", {{"weight", PropertyValue(int64_t(100))}}}});
    ASSERT_EQ(ids.size(), 1U); ASSERT_GT(ids.front(), 64U); auto old = backend.openLogicalGraphReadSnapshot();
    backend.deleteLogicalEdges({{1}, {ids.front()}});
    EXPECT_EQ(old->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0)));
    EXPECT_EQ(old->readEdgeProperty(ids.front(), "weight"), PropertyValue(int64_t(100)));
    old.reset(); backend.close(); EXPECT_EQ(read()[6].size(), 63U); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, JointOwnerDeletionRecoversNativeRowFieldsAndVectorAcrossBothHeaders) {
    seed(false, false, false, false, 64, false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE; uint64_t rowId;
    { RelocatableStorageBackend backend(options); backend.open(); const auto ids = backend.createLogicalNodes({{{"Other"}, {{"age", PropertyValue(int64_t(75))}}}});
      ASSERT_EQ(ids.size(), 1U); rowId = ids.front(); backend.close(); }
    for (const bool complete : {false, true}) {
        RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot(); bool injected = false;
        {
            StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) -> pwrite_ssize_t {
                if (!injected && offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES && count == RSS_SUPERBLOCK_BYTES) {
                    injected = true;
                    if (!complete) return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_BYTES / 2U, offset);
                    (void)portable_pwrite(fd, bytes, count, offset); throw std::runtime_error("joint deletion header interruption");
                }
                return portable_pwrite(fd, bytes, count, offset);
            });
            EXPECT_THROW(backend.deleteLogicalNodes({{64, false, 0}, {rowId, false, 0}}), std::runtime_error);
        }
        ASSERT_TRUE(injected); EXPECT_EQ(old->readNodeProperty(64, "age"), PropertyValue(int64_t(63)));
        EXPECT_EQ(old->readNodeProperty(rowId, "age"), PropertyValue(int64_t(75))); old.reset(); backend.close();
        auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::openReadOnly(io); const auto pin = metadata->openSnapshot();
        EXPECT_EQ(bool(metadata->findNodePageLocationOptional(pin, 64)), !complete);
        EXPECT_EQ(bool(metadata->findNodePageLocationOptional(pin, rowId)), !complete);
        const auto hits = exactVectorSearch({63.0F, 1.0F}); ASSERT_FALSE(hits.empty()); EXPECT_EQ(hits.front().first, complete ? 63U : 64U);
        inspectPublicNativeState();
    }
}
TEST_F(HybridPropertyQueryParityTest, OrdinaryTopologyRefusesUnknownCatalogControlsBeforeChangingLabels) {
    seed(false); addCatalogControl(true); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto before = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); LogicalGraphMutationBatch batch;
    batch.node_label_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {"Solo"}});
    EXPECT_THROW((void)backend.executeLogicalGraphMutationBatch(batch), FormatException); backend.close(); EXPECT_EQ(read(), before);
}
TEST_F(HybridPropertyQueryParityTest, OrdinaryTopologyProofIsNotGrantedToUncoordinatedNativeTransactions) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::open(io); (void)metadata->reserveNativeGraphLogicalIds(1, 0);
    EntityPageStore entities(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY); BTreePageStore trees(pages, *metadata);
    EntityLabelSetStore labels(io, *metadata, pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(pages, *metadata);
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(180);
    options.entity_pool_owner_id = 1; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(180), 1};
    NativeGraphWritePlanner planner(*metadata, entities, labels, roots, trees, options); auto old = metadata->openSnapshot();
    const auto before = old.superblock(); const auto epoch = io->dataMutationEpoch(); auto tx = planner.beginTransaction();
    tx.replaceNodeLabels(1, {}); EXPECT_THROW((void)tx.commit(), FormatException);
    EXPECT_EQ(metadata->openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
    EXPECT_EQ(roots.readEntityPropertyOptional(old, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1)->value, PropertyValue(int64_t(0)));
}
TEST_F(HybridPropertyQueryParityTest, PublicNativePatchAllocatesNamesAndPreservesTheirIdentityAcrossRemoveAndRestore) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    applyPublicNativePatch(backend, {{"born", PropertyValue(int64_t(2026))}, {"city", PropertyValue("Shanghai")}});
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "born"), PropertyValue(int64_t(2026)));
    EXPECT_EQ(current->readNodeProperty(1, "city"), PropertyValue("Shanghai"));
    EXPECT_EQ(old->readNodeProperty(1, "born"), PropertyValue()); current.reset(); old.reset();
    applyPublicNativePatch(backend, {}, {"born"}); applyPublicNativePatch(backend, {{"born", PropertyValue(int64_t(2027))}});
    backend.close(); inspectPublicNativeState();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE; RelocatableStorageBackend reopened(options); reopened.open();
    EXPECT_EQ(reopened.openLogicalGraphReadSnapshot()->readNodeProperty(1, "born"), PropertyValue(int64_t(2027))); reopened.close();
}
TEST_F(HybridPropertyQueryParityTest, PublicNativeReplacementPublishesNewNameAndVectorTogether) {
    seed(false, false, false, false, 64, false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")},
        {"new_temperature", PropertyValue(int64_t(42))},
        {"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(100)), PropertyValue(int64_t(1))})}}}});
    EXPECT_EQ(backend.openLogicalGraphReadSnapshot()->readNodeProperty(1, "new_temperature"), PropertyValue(int64_t(42)));
    backend.close(); const auto results = exactVectorSearch(); ASSERT_FALSE(results.empty()); EXPECT_EQ(results.front().first, 2U); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, InvalidNativeVectorPatchDoesNotPublishItsNewPropertyName) {
    seed(false, false, false, false, 64, false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    EXPECT_THROW(applyPublicNativePatch(backend, {{"new_failure", PropertyValue(int64_t(1))}, {"embedding", PropertyValue("invalid")}}), std::invalid_argument);
    backend.close();
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::openReadOnly(io); SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore trees(pages, *metadata); SystemCatalog catalog(trees, *metadata); auto source = metadata->openSnapshot();
    EXPECT_FALSE(catalog.findPropertyToken(source, "new_failure")); source.release(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NativeConstraintFailureDoesNotPublishItsStagedNewPropertyToken) {
    seed(true, true); convert(EntityPropertyOwnerKind::EPOK_NODE); const auto baseline = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    try { applyPublicNativePatch(backend, {{"new_required_failure", PropertyValue(int64_t(1))}}, {"age"}); FAIL() << "required native field was removed"; }
    catch (const std::runtime_error &error) { EXPECT_NE(std::string(error.what()).find("Constraint violation"), std::string::npos); }
    backend.close(); EXPECT_EQ(read(), baseline);
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::openReadOnly(io); SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore trees(pages, *metadata); SystemCatalog catalog(trees, *metadata); auto source = metadata->openSnapshot();
    EXPECT_FALSE(catalog.findPropertyToken(source, "new_required_failure")); source.release(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NativeNewPropertyTokenIsSharedBy64OwnersInTheOriginalBudget) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    std::vector<LogicalGraphPropertyReplaceRequest> replacements;
    for (uint64_t id = 1; id <= 64; ++id) replacements.push_back({id,
        {{"age", PropertyValue(int64_t(id - 1))}, {"note", PropertyValue(id == 8 ? "seven" : "person")}, {"shared_new", PropertyValue(int64_t(id))}}});
    backend.replaceLogicalNodeProperties(replacements); auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "shared_new"), PropertyValue(int64_t(1)));
    EXPECT_EQ(current->readNodeProperty(64, "shared_new"), PropertyValue(int64_t(64)));
    current.reset(); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, NewNativeTokenFieldsAndVectorRecoverTogetherAcrossBootstrapWriteFaults) {
    seed(false, false, false, false, 64, false, false, true); convert(EntityPropertyOwnerKind::EPOK_NODE);
    for (const bool complete : {false, true}) {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot(); bool injected = false;
        {
            StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) -> pwrite_ssize_t {
                if (!injected && offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES && count == RSS_SUPERBLOCK_BYTES) {
                    injected = true;
                    if (!complete) return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_BYTES / 2U, offset);
                    (void)portable_pwrite(fd, bytes, count, offset); throw std::runtime_error("injected completed bootstrap write interruption");
                }
                return portable_pwrite(fd, bytes, count, offset);
            });
            EXPECT_THROW(backend.replaceLogicalNodeProperties({{1, {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")},
                {"new_fault_name", PropertyValue(int64_t(42))},
                {"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(100)), PropertyValue(int64_t(1))})}}}}), std::runtime_error);
        }
        ASSERT_TRUE(injected); EXPECT_EQ(old->readNodeProperty(1, "new_fault_name"), PropertyValue()); old.reset(); backend.close();
        options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE; RelocatableStorageBackend reopened(options); reopened.open();
        EXPECT_EQ(reopened.openLogicalGraphReadSnapshot()->readNodeProperty(1, "new_fault_name"), complete ? PropertyValue(int64_t(42)) : PropertyValue()); reopened.close();
        const auto results = exactVectorSearch(); ASSERT_FALSE(results.empty()); EXPECT_EQ(results.front().first, complete ? 2U : 1U); inspectPublicNativeState();
    }
}
TEST_F(HybridPropertyQueryParityTest, RawNativeFieldMutationRejectsTokensMissingFromTheCatalog) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::open(io);
    // Fence startup recovery separately from the rejected field publication.
    (void)metadata->reserveNativeGraphLogicalIds(1, 0);
    EntityPageStore entities(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY); BTreePageStore trees(pages, *metadata);
    EntityLabelSetStore labels(io, *metadata, pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY); PropertyCollectionRootStore roots(pages, *metadata);
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(180);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(180), 902};
    NativeGraphWritePlanner planner(*metadata, entities, labels, roots, trees, options);
    auto tx = planner.beginTransaction(); const auto before = tx.baseSuperblock(); const auto epoch = io->dataMutationEpoch();
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024 * 1024);
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{EntityPropertyOwnerKind::EPOK_NODE, 1}, 999, int64_t(1)}}};
    tx.stageHybridPropertyColumns(cells, memory, nullptr, true); EXPECT_THROW((void)tx.commit(), FormatException);
    EXPECT_EQ(metadata->openSnapshot().superblock(), before); EXPECT_EQ(io->dataMutationEpoch(), epoch);
    tx.rollback(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyQueryParityTest, PublicMixedNodeEdgeReplacementPublishesOneNewNameInBothNamespaces) {
    seed(false, true); convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    LogicalGraphMutationBatch batch;
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")}, {"mixed_name", PropertyValue(int64_t(7))}}});
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {{"weight", PropertyValue(int64_t(0))}, {"mixed_name", PropertyValue("edge")}}});
    (void)backend.executeLogicalGraphMutationBatch(batch); auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "mixed_name"), PropertyValue(int64_t(7)));
    EXPECT_EQ(current->readEdgeProperty(1, "mixed_name"), PropertyValue("edge"));
    EXPECT_EQ(old->readNodeProperty(1, "mixed_name"), PropertyValue()); EXPECT_EQ(old->readEdgeProperty(1, "mixed_name"), PropertyValue());
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); EXPECT_EQ(current->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0)));
    current.reset(); old.reset(); backend.close(); inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, PublicMixedNodeVectorAndEdgeColumnPublishTogether) {
    seed(false, false, false, false, 64, false, false, true);
    convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    const PropertyValue embedding(std::vector<PropertyValue>{PropertyValue(int64_t(200)), PropertyValue(int64_t(1))});
    LogicalGraphMutationBatch batch;
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")}, {"embedding", embedding}}});
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {{"weight", PropertyValue(int64_t(1000))}}});
    (void)backend.executeLogicalGraphMutationBatch(batch); auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "embedding"), embedding); EXPECT_EQ(current->readEdgeProperty(1, "weight"), PropertyValue(int64_t(1000)));
    EXPECT_EQ(old->readNodeProperty(1, "embedding"), PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(0)), PropertyValue(int64_t(1))}));
    EXPECT_EQ(old->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0)));
    current.reset(); old.reset(); backend.close(); const auto hits = exactVectorSearch(); ASSERT_FALSE(hits.empty()); EXPECT_EQ(hits.front().first, 2U);
    inspectPublicNativeState();
}
TEST_F(HybridPropertyQueryParityTest, MixedNodeEdgeNewNameAndVectorRecoverTogetherAcrossBothHeaders) {
    seed(false, false, false, false, 64, false, false, true);
    convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    for (const bool complete : {false, true}) {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot(); bool injected = false;
        LogicalGraphMutationBatch batch;
        batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
            {{"age", PropertyValue(int64_t(0))}, {"note", PropertyValue("person")}, {"mixed_fault_name", PropertyValue(int64_t(7))},
                {"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(200)), PropertyValue(int64_t(1))})}}});
        batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
            {{"weight", PropertyValue(int64_t(1000))}, {"mixed_fault_name", PropertyValue("edge")}}});
        {
            StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) -> pwrite_ssize_t {
                if (!injected && offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES && count == RSS_SUPERBLOCK_BYTES) {
                    injected = true;
                    if (!complete) return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_BYTES / 2U, offset);
                    (void)portable_pwrite(fd, bytes, count, offset); throw std::runtime_error("mixed header interruption");
                }
                return portable_pwrite(fd, bytes, count, offset);
            });
            EXPECT_THROW((void)backend.executeLogicalGraphMutationBatch(batch), std::runtime_error);
        }
        ASSERT_TRUE(injected); EXPECT_EQ(old->readNodeProperty(1, "mixed_fault_name"), PropertyValue());
        EXPECT_EQ(old->readEdgeProperty(1, "mixed_fault_name"), PropertyValue()); old.reset(); backend.close();
        options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE; RelocatableStorageBackend reopened(options); reopened.open();
        auto current = reopened.openLogicalGraphReadSnapshot();
        EXPECT_EQ(current->readNodeProperty(1, "mixed_fault_name"), complete ? PropertyValue(int64_t(7)) : PropertyValue());
        EXPECT_EQ(current->readEdgeProperty(1, "mixed_fault_name"), complete ? PropertyValue("edge") : PropertyValue());
        EXPECT_EQ(current->readEdgeProperty(1, "weight"), PropertyValue(int64_t(complete ? 1000 : 0)));
        current.reset(); reopened.close(); const auto hits = exactVectorSearch(); ASSERT_FALSE(hits.empty()); EXPECT_EQ(hits.front().first, complete ? 2U : 1U);
        inspectPublicNativeState();
    }
}
TEST_F(HybridPropertyQueryParityTest, MixedNodeEdgeConstraintFailurePublishesNeitherNamespaceOrNewToken) {
    seed(false, true); convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    LogicalGraphMutationBatch batch;
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {{"note", PropertyValue("person")}, {"failed_mixed_name", PropertyValue(int64_t(7))}}});
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {{"weight", PropertyValue(int64_t(1000))}, {"failed_mixed_name", PropertyValue("edge")}}});
    try { (void)backend.executeLogicalGraphMutationBatch(batch); FAIL() << "required native age was removed"; }
    catch (const std::exception &error) { EXPECT_NE(std::string(error.what()).find("Constraint violation"), std::string::npos) << error.what(); }
    auto current = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(current->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); EXPECT_EQ(current->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0)));
    EXPECT_EQ(current->readNodeProperty(1, "failed_mixed_name"), PropertyValue()); EXPECT_EQ(current->readEdgeProperty(1, "failed_mixed_name"), PropertyValue());
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); current.reset(); old.reset(); backend.close(); inspectPublicNativeState();
    auto file = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
    auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto metadata = RelocatableMetadataStore::openReadOnly(io); SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore trees(pages, *metadata); SystemCatalog catalog(trees, *metadata); auto source = metadata->openSnapshot();
    EXPECT_FALSE(catalog.findPropertyToken(source, "failed_mixed_name"));
}
TEST_F(HybridPropertyQueryParityTest, PublicNativeDetachDeletesItsCompleteIncidentClosureAndPreservesOldPins) {
    seed(true, true); convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    backend.deleteLogicalNodes({{1, true, 8}});
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); EXPECT_EQ(old->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0)));
    EXPECT_EQ(old->readEdgeProperty(2, "weight"), PropertyValue(int64_t(1)));
    auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(current->readNodeProperty(2, "age"), PropertyValue(int64_t(1)));
    current.reset(); old.reset(); backend.close(); inspectPublicNativeState();
    const auto rows = read(); ASSERT_EQ(rows[1].size(), 1U); EXPECT_EQ(rows[1][0][0], PropertyValue(int64_t(63)));
    EXPECT_EQ(rows[1][0][1], PropertyValue(int64_t(2016))); ASSERT_EQ(rows[6].size(), 62U); EXPECT_EQ(rows[6].front()[0], PropertyValue(int64_t(2)));
}
TEST_F(HybridPropertyQueryParityTest, PublicExplicitNodeEdgeDeletionBatchMayRemoveAllIncidentEdgesWithoutDetach) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    LogicalGraphMutationBatch batch; batch.node_deletes.push_back({1, false, 0}); batch.edge_deletes = {{1}, {2}};
    (void)backend.executeLogicalGraphMutationBatch(batch);
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); EXPECT_EQ(old->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0)));
    old.reset(); backend.close(); inspectPublicNativeState(); const auto rows = read(); EXPECT_EQ(rows[1][0][0], PropertyValue(int64_t(63))); EXPECT_EQ(rows[6].size(), 62U);
}
TEST_F(HybridPropertyQueryParityTest, NativeDetachBoundFailureCanRetryTheSameHandleWithoutChangingItsOldSnapshot) {
    seed(false); convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE); const auto before = read();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    EXPECT_THROW(backend.deleteLogicalNodes({{1, true, 1}}), FormatException);
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); EXPECT_EQ(old->readEdgeProperty(2, "weight"), PropertyValue(int64_t(1)));
    backend.deleteLogicalNodes({{1, true, 2}}); old.reset(); backend.close(); inspectPublicNativeState();
    const auto current = read(); EXPECT_EQ(before[6].size(), 64U); EXPECT_EQ(current[6].size(), 62U); EXPECT_EQ(current[1][0][0], PropertyValue(int64_t(63)));
}
TEST_F(HybridPropertyQueryParityTest, NativeDetachUses64AsAnOperationBoundAndCanProgressAfterASeparateEdgeDelete) {
    seed(false, false, false, false, 64, true); convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    EXPECT_THROW(backend.deleteLogicalNodes({{1, true, 64}}), FormatException);
    EXPECT_EQ(old->readEdgeProperty(64, "weight"), PropertyValue(int64_t(63)));
    backend.deleteLogicalEdges({{64}}); backend.deleteLogicalNodes({{1, true, 64}});
    EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0))); old.reset(); backend.close(); inspectPublicNativeState();
    const auto rows = read(); EXPECT_TRUE(rows[6].empty()); EXPECT_EQ(rows[1][0][0], PropertyValue(int64_t(63)));
}
TEST_F(HybridPropertyQueryParityTest, MixedDeletionCanDemoteTheRemainingNodeFieldsAndUpdateScalarIndexes) {
    seed(true, true); convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot();
    LogicalGraphMutationBatch batch; for (uint64_t id = 33; id <= 64; ++id) batch.node_deletes.push_back({id, false, 0}); batch.edge_deletes = {{63}, {64}};
    (void)backend.executeLogicalGraphMutationBatch(batch);
    EXPECT_EQ(old->readNodeProperty(64, "age"), PropertyValue(int64_t(63))); EXPECT_EQ(old->readEdgeProperty(64, "weight"), PropertyValue(int64_t(63)));
    old.reset(); backend.close(); inspectPublicNativeState(); const auto rows = read();
    EXPECT_EQ(rows[1][0][0], PropertyValue(int64_t(32))); EXPECT_EQ(rows[1][0][1], PropertyValue(int64_t(496))); EXPECT_EQ(rows[6].size(), 62U);
    ASSERT_EQ(rows[5].size(), 3U); EXPECT_EQ(rows[5][0][0], PropertyValue(int64_t(31)));
}
TEST_F(HybridPropertyQueryParityTest, NativeDetachEntityFieldsCountsAndVectorRecoverTogetherAcrossBothHeaders) {
    seed(false, true, false, false, 64, false, false, true);
    convert(EntityPropertyOwnerKind::EPOK_NODE); convert(EntityPropertyOwnerKind::EPOK_EDGE);
    for (const bool complete : {false, true}) {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open(); auto old = backend.openLogicalGraphReadSnapshot(); bool injected = false;
        {
            StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) -> pwrite_ssize_t {
                if (!injected && offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES && count == RSS_SUPERBLOCK_BYTES) {
                    injected = true;
                    if (!complete) return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_BYTES / 2U, offset);
                    (void)portable_pwrite(fd, bytes, count, offset); throw std::runtime_error("native detach header interruption");
                }
                return portable_pwrite(fd, bytes, count, offset);
            });
            EXPECT_THROW(backend.deleteLogicalNodes({{1, true, 8}}), std::runtime_error);
        }
        ASSERT_TRUE(injected); EXPECT_EQ(old->readNodeProperty(1, "age"), PropertyValue(int64_t(0)));
        EXPECT_EQ(old->readEdgeProperty(1, "weight"), PropertyValue(int64_t(0))); old.reset(); backend.close(); inspectPublicNativeState();
        const auto rows = read(); EXPECT_EQ(rows[1][0][0], PropertyValue(int64_t(complete ? 63 : 64))); EXPECT_EQ(rows[6].size(), complete ? 62U : 64U);
        const auto hits = exactVectorSearch(); ASSERT_FALSE(hits.empty()); EXPECT_EQ(hits.front().first, complete ? 2U : 1U);
    }
}
#endif
