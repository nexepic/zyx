#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyMapTransactionsTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    static constexpr auto MERGE = NativeHybridPropertyMapMode::NHPMM_MERGE;
    static constexpr auto REPLACE = NativeHybridPropertyMapMode::NHPMM_REPLACE;
    NativeGraphWritePlanner planner(PropertyCollectionRootStore *roots = nullptr) {
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
        return NativeGraphWritePlanner(*metadata_, *entities_, *labels_, roots ? *roots : *properties_, *btree_pages_, options);
    }
    void convert(const std::shared_ptr<MaterializedByteBudget> &memory, EntityPropertyOwnerKind kind = NODE) {
        auto conversion = request(kind, memory); conversion.conversion->publish_candidate = true; run(conversion);
    }
};
}
TEST_F(HybridPropertyMapTransactionsTest, DefaultFormatRejectsMapStagingWithoutChangingTheRowSource) {
#if !ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto writer = planner();
    auto tx = writer.beginTransaction(); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<PropertyCollectionRoot, 1> roots{{{1, graph::Node::typeId, {{1, PropertyValue(int64_t(99))}}}}};
    EXPECT_THROW(tx.stageHybridPropertyMaps(roots, REPLACE, memory), FormatException); EXPECT_TRUE(tx.isActive());
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
#endif
}
TEST_F(HybridPropertyMapTransactionsTest, OwnerProbeIsSnapshotBoundAndDistinguishesPropertylessLiveOwnersFromMissingOwners) {
    seed(NODE, 49, 2); auto row = metadata_->openSnapshot(); EXPECT_FALSE(properties_->isHybridPropertyOwner(row, {NODE, 1}));
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    auto current = metadata_->openSnapshot(); EXPECT_TRUE(properties_->isHybridPropertyOwner(current, {NODE, 1}));
    EXPECT_TRUE(properties_->isHybridPropertyOwner(current, {NODE, 49})); EXPECT_FALSE(properties_->isHybridPropertyOwner(current, {NODE, 60}));
    EXPECT_FALSE(properties_->isHybridPropertyOwner(row, {NODE, 1}));
#endif
    auto foreign = RelocatableMetadataStore::openReadOnly(io_); auto pin = foreign->openSnapshot();
    EXPECT_THROW((void)properties_->isHybridPropertyOwner(pin, {NODE, 1}), FormatException);
    EXPECT_THROW((void)properties_->isHybridPropertyOwner(row, {NODE, 0}), FormatException);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyMapTransactionsTest, MergeOwnsChangedValuesPreservesUnmentionedFieldsAndKeepsStoredNullDistinctFromMissing) {
    seed(NODE, 49, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    auto old = metadata_->openSnapshot(); const auto expectedOld = properties_->readEntity(old, {NODE, 1}); auto writer = planner(); auto tx = writer.beginTransaction();
    std::array<PropertyCollectionRoot, 1> roots{{{1, graph::Node::typeId, {{2, PropertyValue()}, {3, PropertyValue("retained")}}}}};
    ASSERT_NO_THROW(tx.stageHybridPropertyMaps(roots, MERGE, memory)); EXPECT_GT(memory->used(), 0U);
    roots[0].entries[1].value = PropertyValue("caller changed"); ASSERT_TRUE(tx.commit().committed());
    auto current = metadata_->openSnapshot(); const PropertyCollectionRoot expected{1, graph::Node::typeId,
        {{1, PropertyValue(int64_t(1))}, {2, PropertyValue()}, {3, PropertyValue("retained")}}};
    EXPECT_EQ(properties_->readEntity(current, {NODE, 1}), expected); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), expectedOld);
    EXPECT_TRUE(*properties_->readEntityPropertyExists(current, {NODE, 1}, 2)); EXPECT_FALSE(*properties_->readEntityPropertyExists(current, {NODE, 1}, 2, true));
    EXPECT_FALSE(*properties_->readEntityPropertyExists(current, {NODE, 1}, 4)); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyMapTransactionsTest, ReplaceAndEmptyMapRemoveOnlyFieldsAndEmptyMergeCommitsAFencedNoop) {
    seed(NODE, 2, 3); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory)); auto writer = planner();
    const std::array<PropertyCollectionRoot, 1> replaced{{{1, graph::Node::typeId, {{2, PropertyValue(-0.0)}}}}};
    auto first = writer.beginTransaction(); first.stageHybridPropertyMaps(replaced, REPLACE, memory); ASSERT_TRUE(first.commit().committed());
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), replaced[0]);
    const std::array<PropertyCollectionRoot, 1> empty{{{1, graph::Node::typeId, {}}}};
    auto second = writer.beginTransaction(); second.stageHybridPropertyMaps(empty, REPLACE, memory); ASSERT_TRUE(second.commit().committed());
    auto current = metadata_->openSnapshot(); EXPECT_FALSE(properties_->readEntityOptional(current, {NODE, 1}));
    EXPECT_TRUE(properties_->isHybridPropertyOwner(current, {NODE, 1})); EXPECT_NO_THROW((void)entities_->readNode(current, 1));
    const auto before = current.superblock(); const auto epoch = io_->dataMutationEpoch(); auto noop = writer.beginTransaction();
    noop.stageHybridPropertyMaps(empty, MERGE, memory); ASSERT_TRUE(noop.commit().committed());
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyMapTransactionsTest, MapNoopPreservesSignedZeroButRecursiveNaNStillAdvancesTheSemanticFence) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory)); auto writer = planner();
    std::array<PropertyCollectionRoot, 1> roots{{{1, graph::Node::typeId, {{1, PropertyValue(0.0)}}}}};
    auto first = writer.beginTransaction(); first.stageHybridPropertyMaps(roots, REPLACE, memory); ASSERT_TRUE(first.commit().committed());
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch(); roots[0].entries[0].value = PropertyValue(-0.0);
    auto noop = writer.beginTransaction(); noop.stageHybridPropertyMaps(roots, REPLACE, memory); ASSERT_TRUE(noop.commit().committed());
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    roots[0].entries[0].value = PropertyValue(std::vector<PropertyValue>{PropertyValue(std::bit_cast<double>(UINT64_C(0x7ff80000abc12345)))});
    auto nan = writer.beginTransaction(); nan.stageHybridPropertyMaps(roots, REPLACE, memory); ASSERT_TRUE(nan.commit().committed());
    const auto withNan = metadata_->openSnapshot().superblock(); auto again = writer.beginTransaction();
    again.stageHybridPropertyMaps(roots, REPLACE, memory); ASSERT_TRUE(again.commit().committed());
    EXPECT_EQ(metadata_->openSnapshot().superblock().generation.value(), withNan.generation.value() + 1U); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMapTransactionsTest, MoveRollbackAndConflictingCommitRetainOneImmutableRequestAndRefundItsAccount) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory)); auto writer = planner();
    const std::array<PropertyCollectionRoot, 1> roots{{{1, graph::Node::typeId, {{1, PropertyValue("pending")}}}}};
    const auto before = metadata_->openSnapshot().superblock(); auto tx = writer.beginTransaction(); tx.stageHybridPropertyMaps(roots, MERGE, memory);
    auto moved = std::move(tx); EXPECT_FALSE(tx.isActive()); EXPECT_GT(memory->used(), 0U); moved.rollback();
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    auto stale = writer.beginTransaction(); stale.stageHybridPropertyMaps(roots, REPLACE, memory);
    auto competing = writer.beginTransaction(); const std::array<PropertyCollectionRoot, 1> other{{{2, graph::Node::typeId, {{1, PropertyValue(int64_t(999))}}}}};
    competing.stageHybridPropertyMaps(other, MERGE, memory); ASSERT_TRUE(competing.commit().committed());
    EXPECT_FALSE(stale.commit().committed()); EXPECT_EQ(stale.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(1)));
}
TEST_F(HybridPropertyMapTransactionsTest, StageQuotaAndCancellationKeepTheTransactionActiveAndCommitEntryCancellationRetainsTheRequest) {
    struct Cancel final : CancellationCheckpoint { bool stop = false; void check() const override { if (stop) throw std::runtime_error("map cancelled"); } } cancellation;
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory)); auto writer = planner();
    std::array<PropertyCollectionRoot, 1> roots{{{1, graph::Node::typeId, {{1, PropertyValue(std::string(65536, 'x'))}}}}};
    auto tx = writer.beginTransaction(); auto tiny = std::make_shared<MaterializedByteBudget>(1); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW(tx.stageHybridPropertyMaps(roots, MERGE, tiny), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_TRUE(tx.isActive()); EXPECT_EQ(tiny->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    cancellation.stop = true; EXPECT_THROW(tx.stageHybridPropertyMaps(roots, MERGE, memory, &cancellation), std::runtime_error); EXPECT_TRUE(tx.isActive());
    cancellation.stop = false; roots[0].entries[0].value = PropertyValue("retry"); tx.stageHybridPropertyMaps(roots, MERGE, memory, &cancellation);
    cancellation.stop = true; EXPECT_THROW((void)tx.commit(), std::runtime_error); EXPECT_TRUE(tx.isActive()); EXPECT_GT(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    cancellation.stop = false; ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMapTransactionsTest, DuplicateTokensOwnersCrossGroupsAndMoreThan64ActualChangesRefuseBeforePublication) {
    seed(NODE, 128, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory)); auto writer = planner();
    auto tx = writer.beginTransaction(); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<PropertyCollectionRoot, 2> duplicate{{{1, graph::Node::typeId, {}}, {1, graph::Node::typeId, {}}}};
    EXPECT_THROW(tx.stageHybridPropertyMaps(duplicate, MERGE, memory), FormatException);
    const std::array<PropertyCollectionRoot, 2> groups{{{1, graph::Node::typeId, {}}, {65, graph::Node::typeId, {}}}};
    EXPECT_THROW(tx.stageHybridPropertyMaps(groups, MERGE, memory), FormatException);
    const std::array<PropertyCollectionRoot, 1> tokens{{{1, graph::Node::typeId, {{1, PropertyValue()}, {1, PropertyValue()}}}}};
    EXPECT_THROW(tx.stageHybridPropertyMaps(tokens, MERGE, memory), FormatException);
    const std::array<PropertyCollectionRoot, 1> row{{{65, graph::Node::typeId, {{1, PropertyValue(int64_t(99))}}}}};
    EXPECT_THROW(tx.stageHybridPropertyMaps(row, MERGE, memory), FormatException);
    PropertyCollectionRoot wide{1, graph::Node::typeId, {}}; for (uint32_t token = 1; token <= 65; ++token) wide.entries.push_back({token, PropertyValue(int64_t(9999))});
    EXPECT_THROW(tx.stageHybridPropertyMaps(std::span(&wide, 1), MERGE, memory), FormatException);
    EXPECT_TRUE(tx.isActive()); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyMapTransactionsTest, WideMapWithOneChangedValueCopiesOnlyTheFieldAndGroupFrames) {
    seed(NODE, 1, 0); LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto writer = planner(&roots); auto initial = writer.beginTransaction(); PropertyCollectionRoot input{1, graph::Node::typeId, {}};
    for (uint32_t token = 1; token <= 192; ++token) input.entries.push_back({token, PropertyValue(int64_t(token))});
    initial.replaceNodeProperty(std::move(input)); ASSERT_TRUE(initial.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    auto old = metadata_->openSnapshot(); auto root = roots.readEntity(old, {NODE, 1}); root.entries[95].value = PropertyValue(int64_t(123456));
    auto tx = writer.beginTransaction(); tx.stageHybridPropertyMaps(std::span(&root, 1), REPLACE, memory); int64_t frames = 0;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.pool_frames_prepared") *static_cast<int64_t *>(owner) += value;
        }); ASSERT_TRUE(tx.commit().committed());
    }
    EXPECT_GE(frames, 1); EXPECT_LE(frames, 2); EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), root);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}).entries[95].value, PropertyValue(int64_t(96))); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyMapTransactionsTest, EdgeMapReplacementPreservesTheMasterEdgeAndBothAdjacencyDirections) {
    seed(EDGE, 2, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory, EDGE));
    auto old = metadata_->openSnapshot(); const auto edge = entities_->readEdge(old, 1); const auto adjacency = metadata_->findIndexRoot(old, RSS_GLOBAL_ADJACENCY_INDEX_ID);
    const std::array<PropertyCollectionRoot, 1> roots{{{1, graph::Edge::typeId, {{2, PropertyValue("edge")}}}}};
    auto writer = planner(); auto tx = writer.beginTransaction(); tx.stageHybridPropertyMaps(roots, REPLACE, memory); ASSERT_TRUE(tx.commit().committed());
    auto current = metadata_->openSnapshot(); EXPECT_EQ(properties_->readEntity(current, {EDGE, 1}), roots[0]);
    EXPECT_EQ(entities_->readEdge(current, 1), edge); EXPECT_EQ(metadata_->findIndexRoot(current, RSS_GLOBAL_ADJACENCY_INDEX_ID), adjacency);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyMapTransactionsTest, UnchangedLargeMapFieldReusesItsValueAndRemovingItDoesNotClaimGlobalRetirement) {
    seed(NODE, 2, 0); LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto writer = planner(&roots); auto seedTx = writer.beginTransaction();
    seedTx.replaceNodeProperty({1, graph::Node::typeId, {{1, PropertyValue(std::string(8192, 'x'))}, {2, PropertyValue(int64_t(2))}}});
    ASSERT_TRUE(seedTx.commit().committed()); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    auto old = metadata_->openSnapshot(); const auto expectedOld = roots.readEntity(old, {NODE, 1}); auto changed = expectedOld; changed.entries[1].value = PropertyValue(int64_t(999));
    const auto index = metadata_->findIndexRoot(old, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
    const auto manifests = [&] { size_t count = 0; metadata_->visitLargeValueManifestLocations(metadata_->openSnapshot(), [&](const auto &) { ++count; }); return count; };
    const auto count = manifests(); auto tx = writer.beginTransaction(); tx.stageHybridPropertyMaps(std::span(&changed, 1), REPLACE, memory); ASSERT_TRUE(tx.commit().committed());
    auto current = metadata_->openSnapshot(); EXPECT_EQ(roots.readEntity(current, {NODE, 1}), changed);
    EXPECT_EQ(metadata_->findIndexRoot(current, RSS_LARGE_VALUE_CONTENT_INDEX_ID), index); EXPECT_EQ(manifests(), count);
    const std::array<PropertyCollectionRoot, 1> removed{{{1, graph::Node::typeId, {{2, PropertyValue(int64_t(999))}}}}};
    auto erase = writer.beginTransaction(); erase.stageHybridPropertyMaps(removed, REPLACE, memory); ASSERT_TRUE(erase.commit().committed());
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 1}), removed[0]); EXPECT_EQ(manifests(), count);
    EXPECT_EQ(roots.readEntity(old, {NODE, 1}), expectedOld); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyMapTransactionsTest, MergingThe48thHomogeneousMemberDoesNotAutomaticallyPromoteANewField) {
    seed(NODE, 48, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    std::array<PropertyCollectionRoot, 48> roots{};
    for (size_t i = 0; i < roots.size(); ++i) roots[i] = {i + 1U, graph::Node::typeId, {{3, PropertyValue(int64_t(i))}}};
    auto writer = planner(); auto tx = writer.beginTransaction(); tx.stageHybridPropertyMaps(roots, MERGE, memory); ASSERT_TRUE(tx.commit().committed());
    auto current = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory);
    EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 48U); EXPECT_EQ(report.memberships, 96U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
