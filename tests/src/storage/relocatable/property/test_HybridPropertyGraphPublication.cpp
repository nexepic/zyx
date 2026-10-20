#include <gtest/gtest.h>
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectory.hpp"
#include "graph/storage/relocatable/property/HybridPropertyReadContext.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
namespace {
using Request = NativeGraphHybridPropertyRootMutation;
using Mode = NativeHybridPropertyRootMode;
// An ordinary test Index at the reserved owner namespace. These marker entries
// are not field records; the descriptor remains empty or rejects before publish.
constexpr uint64_t MARKER = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_OWNER);
template<class Operation>
void expectError(Operation operation, FormatError expected) {
    try { operation(); FAIL() << "expected a typed native format error"; }
    catch (const FormatException &error) { EXPECT_EQ(error.error(), expected) << error.what(); }
}
class HybridPropertyGraphPublicationTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    Request request() {
        const auto sb = metadata_->openSnapshot().superblock();
        return {sb.database_id, sb.hybrid_property_descriptor_root, Mode::NHPRM_REPLACE, {}, std::nullopt};
    }
    HybridPropertyDescriptorPage descriptor(const MetadataSnapshotPin &pin) {
        MetadataPageFile pages(io_, pin.superblock());
        return pages.readHybridPropertyDescriptor(pin.superblock().hybrid_property_descriptor_root, pin.superblock());
    }
    Superblock publish(const Request &root, uint64_t marker = 0, const MetadataPublishOptions &options = {},
        size_t *preparations = nullptr, bool retireMarker = false, const MetadataSnapshotPin *source = nullptr) {
        std::optional<MetadataSnapshotPin> ownedPin;
        if (!source) ownedPin.emplace(metadata_->openSnapshot());
        const auto &pin = source ? *source : *ownedPin; const auto current = pin.superblock();
        const auto lsn = LogSequenceNumber(current.checkpoint_lsn.value() + 1);
        NativeGraphDerivedStateMutation derived;
        std::optional<BTreeInitialTreeMutationPlan> plan;
        std::optional<BTreeTreeRetirementPlan> retirement;
        if (marker && retireMarker) {
            retirement.emplace(btree_pages_->planTreeRetirement(pin, marker, 32));
            NativeGraphIndexTreeMutation mutation; mutation.index_id = marker;
            mutation.expected_root = retirement->expected_root; mutation.pool_requests = retirement->pool_requests;
            mutation.retire_root = true; derived.index_tree_mutations.push_back(std::move(mutation));
        } else if (marker) {
            const BTreePageEntry entry{{std::byte{1}}, 7};
            plan.emplace(btree_pages_->planInitialTreeMutation(pin, marker, std::span(&entry, 1)));
            NativeGraphIndexTreeMutation mutation; mutation.index_id = marker;
            for (const auto &pool : plan->new_pool_mutations)
                mutation.new_pool_requests.push_back({PhysicalSlotId(40), PhysicalSlotId(80), pool.owner_id, pool.appended_page_count});
            mutation.replacement_root = {plan->root_page.pool_ordinal, plan->root_page.appended_page_ordinal, true};
            derived.index_tree_mutations.push_back(std::move(mutation));
        }
        return metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived, lsn,
            [](auto, auto, auto, auto, auto, auto) {},
            [&](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
                if (plan) {
                    if (preparations) ++*preparations;
                    plan->preparer(batches.at(0), lsn);
                } else if (retirement) {
                    if (preparations) ++*preparations;
                    retirement->preparer(batches.at(0), lsn);
                }
            }, {}, {}, {}, {}, {}, options, {}, current.generation, {}, {}, {}, {}, {}, root);
    }
};
TEST_F(HybridPropertyGraphPublicationTest, FormatFlagGovernsTheBoundedRootRequestWithoutAnExtraPublication) {
    const auto before = metadata_->openSnapshot().superblock(); const auto size = io_->fileSize(); const auto root = request();
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
        const auto after = publish(root);
        EXPECT_EQ(after.generation.value(), before.generation.value() + 1);
        EXPECT_TRUE(after.hybrid_property_descriptor_root.isValid());
        const auto pin = metadata_->openSnapshot(); EXPECT_EQ(descriptor(pin).descriptor.semantic_generation, after.generation);
        HybridPropertyReadContext reader(io_, *slotted_pages_, *metadata_, pin);
        EXPECT_FALSE(reader.visitOwner({EntityPropertyOwnerKind::EPOK_NODE, 1}, 1,
            [](auto, auto) { return true; }).routed);
        class ToggleCancellation final : public CancellationCheckpoint {
        public:
            bool cancelled = false;
            void check() const override { if (cancelled) throw std::logic_error("cancel descriptor-backed point cursor"); }
        } cancellation;
        auto cursor = properties_->openScanCursor(pin, &cancellation);
        cancellation.cancelled = true;
        EXPECT_THROW(cursor.readEntityPropertyOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1), std::logic_error);
        cancellation.cancelled = false;
        EXPECT_FALSE(cursor.readEntityPropertyOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1));
        cancellation.cancelled = true;
        EXPECT_THROW(cursor.readEntityPropertyExists(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1), std::logic_error);
        cancellation.cancelled = false;
        EXPECT_EQ(properties_->readEntityPropertyCount(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}), 0U);
        EXPECT_EQ(properties_->readEntityPropertyExists(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1), false);
        EXPECT_FALSE(cursor.readEntityPropertyExists(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 1));
        EXPECT_TRUE(properties_->readEntityPropertyTokens(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1},
            [](size_t) {}, [](size_t) {}).empty());
        EXPECT_FALSE(properties_->readEntityOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}));
        EXPECT_FALSE(cursor.readEntityOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}));
        EXPECT_THROW(properties_->readEntity(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}), FormatException);
        EXPECT_THROW(cursor.readEntity(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}), FormatException);
        cancellation.cancelled = true;
        EXPECT_THROW(cursor.readEntityOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}), std::logic_error);
        cancellation.cancelled = false;
        EXPECT_FALSE(cursor.readEntityOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, 1}));



    } else {
        EXPECT_THROW(publish(root), FormatException);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->fileSize(), size);
    }
}
TEST_F(HybridPropertyGraphPublicationTest, InvalidRequestsRejectBeforeAnyDerivedPagePreparation) {
    const auto before = metadata_->openSnapshot().superblock(); const auto size = io_->fileSize();
    for (unsigned bad = 0; bad < 6; ++bad) {
        auto root = request();
        if (bad == 0) root.expected_database_id = DatabaseId{};
        if (bad == 1) root.expected_root = {PhysicalSlotId(2), 0, Generation(1)};
        if (bad == 2) root.mode = Mode(255);
        if (bad == 3) root.mode = Mode::NHPRM_CLEAR;
        if (bad == 4) { root.replacement_index_ids[0] = MARKER; root.replacement_index_ids[1] = MARKER; }
        if (bad == 5) {
            root.mode = Mode::NHPRM_CLEAR; root.expected_root = {PhysicalSlotId(2), 1, Generation(1)};
            root.replacement_index_ids[0] = MARKER;
        }
        size_t preparations = 0;
        EXPECT_THROW(publish(root, MARKER, {}, &preparations), FormatException) << bad;
        EXPECT_EQ(preparations, 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        EXPECT_EQ(io_->fileSize(), size);
    }
}
TEST_F(HybridPropertyGraphPublicationTest, OrdinaryScalarStagingCannotClaimAnyReservedFieldNamespace) {
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    const auto before = metadata_->openSnapshot().superblock(); const auto size = io_->fileSize();
    auto transaction = planner.beginTransaction(); const std::array key{std::byte{1}};
    const BTreePageEntry entry{{std::byte{1}}, 7};
    for (size_t ordinal = 0; ordinal < RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT; ++ordinal) {
        const auto id = hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(ordinal));
        expectError([&] { transaction.stageScalarIndexUpsert(*btree_pages_, id, key, 7); }, FormatError::FERR_INVALID_HEADER);
        expectError([&] { transaction.stageScalarIndexErase(*btree_pages_, id, key); }, FormatError::FERR_INVALID_HEADER);
        expectError([&] { transaction.stageScalarIndexCreate(*btree_pages_, id, std::span(&entry, 1)); }, FormatError::FERR_INVALID_HEADER);
        auto plan = btree_pages_->planInitialTreeMutation(transaction.baseSnapshot(), id, std::span(&entry, 1));
        expectError([&] { transaction.stageScalarIndexCreate(*btree_pages_, std::move(plan)); }, FormatError::FERR_INVALID_HEADER);
        expectError([&] { transaction.stageScalarIndexDrop(*btree_pages_, id, 1); }, FormatError::FERR_INVALID_HEADER);
    }
    transaction.rollback(); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->fileSize(), size);
}
TEST_F(HybridPropertyGraphPublicationTest, TreePoolEpochAuthenticatesExactlyAndIsIndependentOfMetadataVisibility) {
    PageOwnerHeader owner;
    owner.page_id = PageId(101); owner.owner_id = MARKER; owner.kind = PageOwnerKind::POK_INDEX;
    owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
    owner.generation = Generation(99); owner.committed_lsn = LogSequenceNumber(2);
    BTreePageHeader header;
    header.index_id = MARKER; header.page_id = owner.page_id; header.generation = owner.generation;
    auto page = BTreePage::create(owner, header);
    const std::array key{std::byte{1}}; page.insert({{key.begin(), key.end()}, 7});
    SlottedPagePoolAllocationRequest allocation;
    allocation.segment_id = SegmentId(71); allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
    allocation.pool_owner_id = MARKER; allocation.generation = owner.generation;
    allocation.page_id = owner.page_id; allocation.page_owner_id = MARKER;
    allocation.page_owner_kind = PageOwnerKind::POK_INDEX;
    ASSERT_TRUE(btree_pages_->allocate(allocation, page, PhysicalSlotId(40), PhysicalSlotId(80),
        owner.committed_lsn).has_value());
    const IndexRootLocation root{MARKER, owner.page_id, owner.generation};
    static_cast<void>(btree_pages_->publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
    auto old = metadata_->openSnapshot();
    ASSERT_LT(old.superblock().generation.value(), root.root_generation.value());
    BTreeIndex index(MARKER, *btree_pages_, *metadata_); EXPECT_EQ(index.find(old, key), 7U);
    HybridPropertyDescriptorPage frame;
    frame.generation = old.superblock().generation; frame.committed_lsn = old.superblock().checkpoint_lsn;
    frame.descriptor.database_id = old.superblock().database_id;
    frame.descriptor.semantic_generation = frame.generation;
    frame.descriptor.roots[0] = {root.root_page_id, root.root_generation};
    EXPECT_EQ(HybridPropertyDescriptorCodec::decode(HybridPropertyDescriptorCodec::encode(frame),
        frame.descriptor.database_id, frame.generation, frame.generation, frame.committed_lsn), frame);
    const auto binding = HybridPropertyDirectoryCodec::bindRoot(frame.descriptor, old,
        HybridPropertyRootOrdinal::HPRO_NODE_OWNER);
    BTreeIndex bound(MARKER, *btree_pages_, *metadata_, BTreeLeafValueLayout::BLVL_U64, binding);
    EXPECT_EQ(bound.find(old, key), 7U);
    auto foreign = frame.descriptor; foreign.database_id = DatabaseId(frame.descriptor.database_id.value() + 1);
    EXPECT_THROW(HybridPropertyDirectoryCodec::bindRoot(foreign, old, HybridPropertyRootOrdinal::HPRO_NODE_OWNER), FormatException);
    foreign = frame.descriptor; foreign.semantic_generation = Generation(old.superblock().generation.value() + 1);
    EXPECT_THROW(HybridPropertyDirectoryCodec::bindRoot(foreign, old, HybridPropertyRootOrdinal::HPRO_NODE_OWNER), FormatException);
    EXPECT_THROW(HybridPropertyDirectoryCodec::bindRoot(frame.descriptor, old, HybridPropertyRootOrdinal::HPRO_NODE_COLUMN), FormatException);
    EXPECT_THROW(HybridPropertyDirectoryCodec::bindRoot(frame.descriptor, old, HybridPropertyRootOrdinal(255)), FormatException);
    foreign = frame.descriptor; foreign.roots[1] = foreign.roots[0];
    const auto wrongRole = HybridPropertyDirectoryCodec::bindRoot(foreign, old, HybridPropertyRootOrdinal::HPRO_NODE_COLUMN);
    BTreeIndex roleBound(wrongRole.root.index_id, *btree_pages_, *metadata_, BTreeLeafValueLayout::BLVL_U64, wrongRole);
    EXPECT_THROW(roleBound.find(old, key), FormatException);
    auto wrong = root; wrong.root_generation = Generation(100);
    expectError([&] { static_cast<void>(btree_pages_->publishRoots(std::span(&wrong, 1), {},
        LogSequenceNumber(4))); }, FormatError::FERR_INVALID_STATE);
    // The typed publisher retains the old authoritative directory on refusal.
    auto current = metadata_->openSnapshot();
    EXPECT_EQ(current.superblock(), old.superblock());
    EXPECT_EQ(index.find(current, key), 7U);
    EXPECT_EQ(index.find(old, key), 7U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyGraphPublicationTest, DescriptorNamespaceCannotBindAnotherRoleOrAnOrdinaryIndexBeforePreparation) {
    const auto before = metadata_->openSnapshot().superblock(); const auto size = io_->fileSize();
    for (size_t ordinal = 0; ordinal < RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT; ++ordinal) {
        auto root = request();
        root.replacement_index_ids[ordinal] = hybridPropertyIndexId(
            static_cast<HybridPropertyRootOrdinal>((ordinal + 1) % RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT));
        size_t preparations = 0;
        expectError([&] { publish(root, MARKER, {}, &preparations); }, FormatError::FERR_INVALID_HEADER);
        EXPECT_EQ(preparations, 0U);
    }
    for (const auto id : std::array<uint64_t, 4>{7, UINT64_C(0x8000000000000002),
        UINT64_C(0x8000000000000020), UINT64_MAX}) {
        auto root = request(); root.replacement_index_ids[0] = id; size_t preparations = 0;
        expectError([&] { publish(root, MARKER, {}, &preparations); }, FormatError::FERR_INVALID_HEADER);
        EXPECT_EQ(preparations, 0U);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->fileSize(), size);
}
TEST_F(HybridPropertyGraphPublicationTest, OrdinaryIndexDataAndDescriptorPublishInOneGenerationAndOldPinSurvives) {
    auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    size_t preparations = 0; const auto after = publish(request(), MARKER, {}, &preparations);
    EXPECT_EQ(preparations, 1U); EXPECT_EQ(after.generation.value(), before.generation.value() + 1);
    EXPECT_EQ(after.checkpoint_lsn.value(), before.checkpoint_lsn.value() + 1);
    BTreeIndex marker(MARKER, *btree_pages_, *metadata_); const std::array key{std::byte{1}};
    auto fresh = metadata_->openSnapshot(); EXPECT_FALSE(marker.find(old, key)); EXPECT_EQ(marker.find(fresh, key), 7U);
    EXPECT_EQ(descriptor(fresh).descriptor.semantic_generation, after.generation);
    EXPECT_EQ(descriptor(fresh).generation, after.generation);
    EXPECT_EQ(before.entity_property_directory_root, after.entity_property_directory_root);
    old.release(); fresh.release(); reopenNativeStores();
    auto recovered = metadata_->openSnapshot(); BTreeIndex reopened(MARKER, *btree_pages_, *metadata_);
    EXPECT_EQ(reopened.find(recovered, key), 7U); EXPECT_EQ(descriptor(recovered).descriptor.semantic_generation, after.generation);
}
TEST_F(HybridPropertyGraphPublicationTest, ClearSharesThePublicationAndOldDescriptorRemainsReadable) {
    publish(request()); auto old = metadata_->openSnapshot(); const auto previous = descriptor(old);
    auto clear = request(); clear.mode = Mode::NHPRM_CLEAR;
    const auto after = publish(clear, MARKER); EXPECT_EQ(after.generation.value(), old.superblock().generation.value() + 1);
    EXPECT_EQ(after.hybrid_property_descriptor_root, PhysicalPageRef{}); EXPECT_EQ(descriptor(old), previous);
    auto fresh = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->findIndexRootOptional(fresh, MARKER));
    old.release(); fresh.release(); reopenNativeStores();
    EXPECT_EQ(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root, PhysicalPageRef{});
}
TEST_F(HybridPropertyGraphPublicationTest, WrongDatabaseOrStaleRootFencesBeforePreparingGraphData) {
    const auto stale = request(); publish(stale); const auto current = metadata_->openSnapshot().superblock();
    size_t preparations = 0;
    EXPECT_THROW(publish(stale, MARKER, {}, &preparations), FormatException);
    auto wrong = request(); wrong.expected_database_id = DatabaseId(current.database_id.value() + 1);
    EXPECT_THROW(publish(wrong, MARKER, {}, &preparations), FormatException);
    EXPECT_EQ(preparations, 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), current);
}
TEST_F(HybridPropertyGraphPublicationTest, UninstalledNonemptyRootsAbortAlreadyPreparedDataWithoutPublishingIt) {
    auto root = request(); root.replacement_index_ids[0] = MARKER;
    const auto before = metadata_->openSnapshot().superblock(); size_t preparations = 0;
    expectError([&] { publish(root, MARKER, {}, &preparations); }, FormatError::FERR_UNSUPPORTED_VERSION);
    EXPECT_EQ(preparations, 1U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_FALSE(metadata_->findIndexRootOptional(metadata_->openSnapshot(), MARKER));
    reopenNativeStores(); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_FALSE(metadata_->findIndexRootOptional(metadata_->openSnapshot(), MARKER));
    EXPECT_NO_THROW(publish(request(), MARKER));
}
TEST_F(HybridPropertyGraphPublicationTest, MissingRetainedTreeFailsClosedWithoutManufacturingDescriptorReferences) {
    auto root = request(); root.replacement_index_ids[0] = MARKER;
    const auto before = metadata_->openSnapshot().superblock();
    expectError([&] { publish(root); }, FormatError::FERR_INVALID_STATE);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyGraphPublicationTest, RetainedRootResolvesButNonemptyRoutingStillCannotBePublished) {
    publish(request(), MARKER); const auto before = metadata_->openSnapshot().superblock();
    auto root = request(); root.replacement_index_ids[0] = MARKER; size_t preparations = 0;
    expectError([&] { publish(root, 0, {}, &preparations); }, FormatError::FERR_UNSUPPORTED_VERSION);
    EXPECT_EQ(preparations, 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    BTreeIndex marker(MARKER, *btree_pages_, *metadata_); const std::array key{std::byte{1}};
    auto current = metadata_->openSnapshot(); EXPECT_EQ(marker.find(current, key), 7U);
}
TEST_F(HybridPropertyGraphPublicationTest, DescriptorCannotBindATreeRetiredByTheSameCandidate) {
    publish(request(), MARKER); const auto before = metadata_->openSnapshot().superblock();
    auto root = request(); root.replacement_index_ids[0] = MARKER; size_t preparations = 0;
    expectError([&] { publish(root, MARKER, {}, &preparations, true); }, FormatError::FERR_INVALID_STATE);
    EXPECT_EQ(preparations, 1U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    BTreeIndex marker(MARKER, *btree_pages_, *metadata_); const std::array key{std::byte{1}};
    auto current = metadata_->openSnapshot(); EXPECT_EQ(marker.find(current, key), 7U); current.release();
    reopenNativeStores(); auto recovered = metadata_->openSnapshot(); BTreeIndex reopened(MARKER, *btree_pages_, *metadata_);
    EXPECT_EQ(reopened.find(recovered, key), 7U); EXPECT_EQ(recovered.superblock(), before);
}
TEST_F(HybridPropertyGraphPublicationTest, WriterAuthenticatesTheExpectedOldRootBeforePreparingUnrelatedData) {
    publish(request()); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    Request root{before.database_id, before.hybrid_property_descriptor_root, Mode::NHPRM_CLEAR, {}};
    const auto original = descriptor(pin); auto wrong = original;
    wrong.descriptor.database_id = DatabaseId(before.database_id.value() + 1);
    MetadataPageFile pages(io_, before); const auto offset = pages.pageOffset(before.hybrid_property_descriptor_root);
    const auto corrupted = HybridPropertyDescriptorCodec::encode(wrong);
    io_->writeAt({offset, before.hybrid_property_descriptor_root.generation.value()}, corrupted.data(), corrupted.size()); io_->sync();
    EXPECT_THROW((HybridPropertyReadContext(io_, *slotted_pages_, *metadata_, pin)), FormatException);
    size_t preparations = 0;
    expectError([&] { publish(root, MARKER, {}, &preparations, false, &pin); }, FormatError::FERR_INVALID_HEADER);
    EXPECT_EQ(preparations, 0U);
    const auto restored = HybridPropertyDescriptorCodec::encode(original);
    io_->writeAt({offset, before.hybrid_property_descriptor_root.generation.value()}, restored.data(), restored.size()); io_->sync();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_FALSE(metadata_->findIndexRootOptional(metadata_->openSnapshot(), MARKER));
    EXPECT_NO_THROW(publish(root, MARKER));
}
TEST_F(HybridPropertyGraphPublicationTest, EachBootstrapInterruptionRecoversACompleteDataAndDescriptorPair) {
    uint64_t markerId = MARKER;
    for (auto failpoint : {BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE,
        BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = failpoint;
        EXPECT_THROW(publish(request(), markerId, options), BootstrapPublishInterrupted);
        reopenNativeStores(); auto recovered = metadata_->openSnapshot();
        const bool completeNew = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(metadata_->findIndexRootOptional(recovered, markerId).has_value(), completeNew);
        EXPECT_EQ(recovered.superblock().hybrid_property_descriptor_root.isValid(), completeNew);
        if (completeNew) EXPECT_EQ(descriptor(recovered).descriptor.semantic_generation, recovered.superblock().generation);
        ++markerId;
    }
}

TEST_F(HybridPropertyGraphPublicationTest, EmptyDescriptorKeepsActualRowBatchAndCursorContracts) {
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto transaction = planner.beginTransaction(); transaction.reserveNodeIds(2);
    transaction.createNodeWithReservedId(1, {}); transaction.createNodeWithReservedId(2, {});
    transaction.replaceNodeProperty({1, graph::Node::typeId,
        {{1, graph::PropertyValue(int64_t(100))}, {2, graph::PropertyValue("row-field")}}});
    ASSERT_TRUE(transaction.commit().committed());
    publish(request()); const auto pin = metadata_->openSnapshot();
    const std::array<EntityPropertyKey, 2> owners{{{EntityPropertyOwnerKind::EPOK_NODE, 1},
        {EntityPropertyOwnerKind::EPOK_NODE, 2}}};
    const std::array<uint32_t, 3> tokens{1, 2, 3};
    std::array<std::optional<PropertyCollectionRootEntry>, 6> values{};
    std::array<std::optional<size_t>, 2> counts{};
    auto check = [&] {
        ASSERT_TRUE(values[0]); EXPECT_EQ(values[0]->value, graph::PropertyValue(int64_t(100)));
        ASSERT_TRUE(values[1]); EXPECT_EQ(values[1]->value, graph::PropertyValue("row-field"));
        for (size_t i = 2; i < values.size(); ++i) EXPECT_FALSE(values[i]);
    };
    properties_->readEntityPropertiesOptionalSorted(pin, owners, tokens, values); check();
    properties_->readEntityPropertiesAndCountsOptionalSorted(pin, owners, tokens, values, counts); check();
    EXPECT_EQ(counts, (std::array<std::optional<size_t>, 2>{2, 0}));
    properties_->readEntityPropertyCountsSorted(pin, owners, counts);
    EXPECT_EQ(counts, (std::array<std::optional<size_t>, 2>{2, 0}));
    std::array<std::optional<PropertyCollectionRootEntry>, 2> single{};
    properties_->readEntityPropertiesOptionalSorted(pin, owners, 2, single);
    ASSERT_TRUE(single[0]); EXPECT_EQ(single[0]->value, graph::PropertyValue("row-field")); EXPECT_FALSE(single[1]);
    auto cursor = properties_->openScanCursor(pin);
    cursor.readEntityPropertiesOptionalSorted(pin, owners, tokens, values); check();
    cursor.readEntityPropertiesAndCountsOptionalSorted(pin, owners, tokens, values, counts); check();
    EXPECT_EQ(counts, (std::array<std::optional<size_t>, 2>{2, 0}));
    cursor.readEntityPropertyCountsSorted(pin, owners, counts);
    EXPECT_EQ(counts, (std::array<std::optional<size_t>, 2>{2, 0}));
    size_t emitted = 0;
    std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> sink =
        [&](size_t owner, size_t token, std::optional<PropertyCollectionRootEntry> value) {
            EXPECT_EQ(owner, 0U); EXPECT_EQ(token, emitted++); EXPECT_EQ(value, values[token]);
        };
    cursor.readEntityPropertiesOptionalSorted(pin, owners, tokens, {}, true, nullptr, &sink);
    EXPECT_EQ(emitted, 3U);
    EXPECT_EQ(properties_->readEntity(pin, owners[0]), cursor.readEntity(pin, owners[0]));
    EXPECT_FALSE(properties_->readEntityOptional(pin, owners[1]));
    EXPECT_FALSE(cursor.readEntityOptional(pin, owners[1]));
}
#endif
} // namespace
