#include <gtest/gtest.h>
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/property/HybridPropertyGroupConversion.hpp"
#include "graph/storage/relocatable/property/HybridPropertyIndexIds.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
using Request = NativeGraphHybridPropertyRootMutation;
constexpr auto NODE = EntityPropertyOwnerKind::EPOK_NODE;
constexpr auto EDGE = EntityPropertyOwnerKind::EPOK_EDGE;
class HybridPropertyOrdinaryConversionTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    void seed(EntityPropertyOwnerKind kind, size_t owners = 49, size_t fields = 2) {
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
        auto transaction = planner.beginTransaction();
        const auto firstNode = metadata_->openSnapshot().superblock().next_logical_node_id;
        const auto firstEdge = metadata_->openSnapshot().superblock().next_logical_edge_id;
        transaction.reserveNodeIds(kind == NODE ? owners : 2);
        uint64_t source = 0, target = 0;
        if (kind == EDGE) {
            source = transaction.createNodeWithReservedId(firstNode, {});
            target = transaction.createNodeWithReservedId(firstNode + 1, {}); transaction.reserveEdgeIds(owners);
        }
        for (size_t i = 0; i < owners; ++i) {
            const auto id = kind == NODE ? transaction.createNodeWithReservedId(firstNode + i, {}) :
                transaction.createEdgeWithReservedId(firstEdge + i, source, target, 7);
            if (i == 48 && owners == 49) continue; // live owner without a Property root
            PropertyCollectionRoot root{id, kind == NODE ? graph::Node::typeId : graph::Edge::typeId, {}};
            for (size_t token = 1; token <= fields; ++token) root.entries.push_back({uint32_t(token), PropertyValue(int64_t(i + token))});
            if (kind == NODE) transaction.replaceNodeProperty(std::move(root));
            else transaction.replaceEdgeProperty(std::move(root));
        }
        ASSERT_TRUE(transaction.commit().committed());
    }
    Request request(EntityPropertyOwnerKind kind, std::shared_ptr<std::pmr::memory_resource> memory) {
        const auto current = metadata_->openSnapshot().superblock();
        return {current.database_id, current.hybrid_property_descriptor_root, NativeHybridPropertyRootMode::NHPRM_REPLACE,
            {}, NativeGraphHybridPropertyConversion{kind, 1, std::move(memory), nullptr, PhysicalSlotId(40), PhysicalSlotId(80), 903}};
    }
    void run(const Request &request, bool omitFence = false, std::optional<Generation> expected = {}) {
        const auto current = metadata_->openSnapshot().superblock();
        NativeGraphDerivedStateMutation derived;
        static_cast<void>(metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived,
            LogSequenceNumber(current.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {},
            [](auto &) {}, {}, {}, {}, {}, {}, {}, {},
            omitFence ? std::nullopt : std::optional(expected.value_or(current.generation)), {}, {}, {}, {}, {}, request));
    }
    struct PreparationStats { int64_t fields = -1, indexes = -1, pool_bytes = -1; };
    PreparationStats prepareWithStats(const Request &request) {
        PreparationStats stats;
        graph::debug::PerfTrace::ScopedObserver observer(&stats,
            [](void *owner, std::string_view key, int64_t value, bool) noexcept {
                auto &result = *static_cast<PreparationStats *>(owner);
                if (key == "native_graph.hybrid.private_field_frames") result.fields = value;
                if (key == "native_graph.hybrid.private_index_frames") result.indexes = value;
                if (key == "native_graph.hybrid.private_data_pool_bytes") result.pool_bytes = value;
            });
        expectPreparedRefusal(request); return stats;
    }
    void expectPreparedRefusal(const Request &request) {
        try { run(request); FAIL() << "private conversion was published"; }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION) << error.what();
            EXPECT_NE(std::string(error.what()).find("private field preparation completed"), std::string::npos) << error.what();
            EXPECT_NE(std::string(error.what()).find("directory plans prepared"), std::string::npos) << error.what();
            EXPECT_NE(std::string(error.what()).find("physical images and final roots prepared"), std::string::npos) << error.what();
        }
    }
};
TEST_F(HybridPropertyOrdinaryConversionTest, FormatFlagControlsTheTypedOrdinaryRequestBeforeAnyPublication) {
    seed(NODE, 2); const auto before = metadata_->openSnapshot().superblock();
    const auto bytes = io_->fileSize(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); const auto conversion = request(NODE, memory);
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) expectPreparedRefusal(conversion);
    else EXPECT_THROW(run(conversion), FormatException);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOrdinaryConversionTest, WriterAuthenticatesNodeAndEdgeRowsIncludingPropertylessLiveOwners) {
    for (const auto kind : {NODE, EDGE}) {
        seed(kind); auto old = metadata_->openSnapshot(); const auto before = old.superblock();
        const auto original = properties_->readEntity(old, {kind, 1}); const auto bytes = io_->fileSize();
        const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        expectPreparedRefusal(request(kind, memory));
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        EXPECT_EQ(properties_->readEntity(old, {kind, 1}), original);
        old.release(); reopenNativeStores(); auto reopened = metadata_->openSnapshot();
        EXPECT_EQ(reopened.superblock(), before); EXPECT_EQ(properties_->readEntity(reopened, {kind, 1}), original);
    }
}
TEST_F(HybridPropertyOrdinaryConversionTest, InvalidGroupAccountModeOrFenceRejectsBeforeSourceReads) {
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    for (unsigned bad = 0; bad < 11; ++bad) {
        auto memory = std::make_shared<MaterializedByteBudget>(bad == 4 ? 2 * 1024 * 1024 : 1024 * 1024);
        auto conversion = request(NODE, memory);
        if (bad == 0) conversion.conversion->group_id = 0;
        if (bad == 1) conversion.conversion->group_id = RSS_HYBRID_MAX_GROUP_ID + 1;
        if (bad == 2) conversion.conversion->owner_kind = EntityPropertyOwnerKind(255);
        if (bad == 3) conversion.conversion->memory.reset();
        if (bad == 5) {
            conversion.mode = NativeHybridPropertyRootMode::NHPRM_CLEAR;
            conversion.expected_root = {PhysicalSlotId(2), 1, Generation(1)};
        }
        if (bad == 8) conversion.conversion->first_slot = {};
        if (bad == 9) conversion.conversion->last_slot = PhysicalSlotId(39);
        if (bad == 10) conversion.conversion->field_pool_owner_id = 0;
        if (bad == 6) conversion.replacement_index_ids[0] = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_OWNER);
        try { run(conversion, bad == 7); FAIL() << bad; }
        catch (const FormatException &error) { EXPECT_EQ(error.error(), FormatError::FERR_INVALID_HEADER) << bad; }
        EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
class CancelNow final : public CancellationCheckpoint {
public: void check() const override { throw std::runtime_error("cancel ordinary native conversion"); }
};
TEST_F(HybridPropertyOrdinaryConversionTest, QuotaCancellationStaleFenceAndEmptyGroupRefundWithoutChangingTheFile) {
    seed(NODE, 2); const auto before = metadata_->openSnapshot().superblock();
    const auto bytes = io_->fileSize(); const auto epoch = io_->dataMutationEpoch();
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW(run(request(NODE, tiny)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); CancelNow cancellation; conversion.conversion->cancellation = &cancellation;
    EXPECT_THROW(run(conversion), std::runtime_error); EXPECT_EQ(memory->used(), 0U);
    conversion = request(NODE, memory);
    EXPECT_THROW(run(conversion, false, Generation(before.generation.value() - 1)), FormatException);
    conversion.conversion->group_id = 2;
    try { run(conversion); FAIL() << "empty group accepted"; }
    catch (const FormatException &error) { EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE); }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    {
        graph::query::execution::MaterializedByteReservation retained(memory.get()); retained.set(128);
        expectPreparedRefusal(request(NODE, memory)); EXPECT_EQ(memory->used(), 128U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, PhysicalDestinationExhaustionRefundsAfterPartialPrivateBindingAndCanRetry) {
    seed(NODE, 2);
    const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto original = properties_->readEntity(pin, {NODE, 1});
    const auto epoch = io_->dataMutationEpoch(); const auto bytes = io_->fileSize();
    uint64_t first = 0;
    for (uint64_t slot = 40; slot <= 77 && !first; ++slot) {
        bool clear = true;
        for (uint64_t i = 0; i < 4; ++i)
            clear = clear && !metadata_->isPhysicalSlotAllocated(pin, PhysicalSlotId(slot + i));
        if (clear) first = slot;
    }
    ASSERT_NE(first, 0U);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    for (uint64_t available = 1; available < 4; ++available) {
        auto conversion = request(NODE, memory);
        conversion.conversion->first_slot = PhysicalSlotId(first);
        conversion.conversion->last_slot = PhysicalSlotId(first + available - 1);
        try { run(conversion); FAIL() << "placement exceeded its reservation range"; }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
            EXPECT_NE(std::string(error.what()).find("placement has no free slot"), std::string::npos);
        }
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
    }
    auto retry = request(NODE, memory);
    retry.conversion->first_slot = PhysicalSlotId(first); retry.conversion->last_slot = PhysicalSlotId(first + 3);
    const auto stats = prepareWithStats(retry);
    EXPECT_EQ(stats.fields, 1); EXPECT_EQ(stats.indexes, 3);
    EXPECT_EQ(stats.pool_bytes, 4 * int64_t(RSS_SEGMENT_SIZE));
    const auto after = metadata_->openSnapshot();
    for (uint64_t i = 0; i < 4; ++i) EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(after, PhysicalSlotId(first + i)));
    EXPECT_EQ(properties_->readEntity(after, {NODE, 1}), original);
    EXPECT_EQ(after.superblock(), before); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
}
class PrivatePreparationCancelled final : public std::runtime_error {
public: PrivatePreparationCancelled() : std::runtime_error("cancel after private physical preparation") {}
};
class CancelAfterPrivatePreparation final : public CancellationCheckpoint {
public:
    bool prepared = false;
    void check() const override { if (prepared) throw PrivatePreparationCancelled(); }
};
TEST_F(HybridPropertyOrdinaryConversionTest, CancellationAfterFinalRootPreparationRefundsEveryPrivateImageAndCanRetry) {
    seed(NODE, 2);
    const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto original = properties_->readEntity(pin, {NODE, 1});
    const auto epoch = io_->dataMutationEpoch(); const auto bytes = io_->fileSize();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    CancelAfterPrivatePreparation cancellation;
    auto conversion = request(NODE, memory); conversion.conversion->cancellation = &cancellation;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&cancellation,
            [](void *owner, std::string_view key, int64_t, bool) noexcept {
                if (key == "native_graph.hybrid.private_data_pool_bytes")
                    static_cast<CancelAfterPrivatePreparation *>(owner)->prepared = true;
            });
        EXPECT_THROW(run(conversion), PrivatePreparationCancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}), original);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
    expectPreparedRefusal(request(NODE, memory)); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, CompleteBoundedDenseGroupPreparesAllDirectoryPlansWithinTheConversionAccount) {
    seed(NODE, 64, 64);
    const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto original = properties_->readEntity(pin, {NODE, 64});
    ASSERT_EQ(original.entries.size(), 64U);
    const auto epoch = io_->dataMutationEpoch(); const auto bytes = io_->fileSize();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    // Compare actual whole data-pool reservations with the actual ROW pools
    // touched by this fixture. This excludes metadata/retirement and is not a
    // complete layout admission or a latency benchmark.
    std::array<SegmentId, 64> rowPools;
    for (uint64_t id = 1; id <= 64; ++id) {
        const auto root = metadata_->findEntityPropertyLocation(pin, {NODE, id});
        rowPools[id - 1] = metadata_->findPageLocation(pin, root.page_id).segment_id;
    }
    std::sort(rowPools.begin(), rowPools.end());
    const auto rowPoolCount = size_t(std::unique(rowPools.begin(), rowPools.end()) - rowPools.begin());
    const auto stats = prepareWithStats(request(NODE, memory));
    EXPECT_GT(stats.fields, 0); EXPECT_GT(stats.indexes, 0);
    EXPECT_GT(stats.pool_bytes, int64_t(rowPoolCount * RSS_SEGMENT_SIZE));
    EXPECT_EQ(stats.pool_bytes % RSS_SEGMENT_SIZE, 0);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 64}), original);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
}
TEST_F(HybridPropertyOrdinaryConversionTest, MembershipPreflightRejectsBeforeDecodingAnOverBoundSource) {
    seed(NODE, 64, 65); const auto before = metadata_->openSnapshot().superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto bytes = io_->fileSize(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW(run(request(NODE, memory)), std::length_error);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyOrdinaryConversionTest, ChecksummedForeignRowOwnerCannotBecomeAConversionSource) {
    seed(NODE, 2); seed(EDGE); auto old = metadata_->openSnapshot();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const auto location = metadata_->findEntityPropertyLocation(old, {NODE, 1});
    auto page = slotted_pages_->read(old, location.page_id); auto foreign = original; foreign.owner_entity_id = 999;
    page.overwrite({location.page_id, location.slot_id, location.slot_generation}, EntityPropertyCollectionRootCodec::encode(foreign));
    static_cast<void>(slotted_pages_->replacePage(page, LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1)));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    // Edge conversion must not decode an unrelated corrupt Node property root.
    expectPreparedRefusal(request(EDGE, memory));
    try { run(request(NODE, memory)); FAIL() << "foreign owner accepted"; }
    catch (const FormatException &error) { EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE); }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
}
TEST_F(HybridPropertyOrdinaryConversionTest, RowConversionRejectsAnExistingDirectoryEvenWhenItsNewDirectoryWouldBeAbsent) {
    seed(NODE);
    const auto index = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_EXTENSION);
    const auto source = metadata_->openSnapshot().superblock();
    OrderedBTreeEntrySource entries = []() -> OrderedBTreeEntryCursor {
        return [done = false]() mutable -> std::optional<BTreePageEntry> {
            if (done) return {};
            done = true; return BTreePageEntry{{std::byte{1}}, 0};
        };
    };
    static_cast<void>(BulkBTreeBuilder::build(*btree_pages_, *metadata_, index, entries,
        {PhysicalSlotId(40), PhysicalSlotId(80), index, 1, 1, BTreeLeafValueLayout::BLVL_EMPTY},
        LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto root = metadata_->findIndexRoot(pin, index);
    const auto original = properties_->readEntity(pin, {NODE, 1});
    const auto epoch = io_->dataMutationEpoch(); const auto bytes = io_->fileSize();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    try { run(request(NODE, memory)); FAIL() << "an orphaned field namespace was overwritten"; }
    catch (const FormatException &error) {
        EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
        EXPECT_NE(std::string(error.what()).find("existing hybrid directory"), std::string::npos);
    }
    const auto after = metadata_->openSnapshot();
    EXPECT_EQ(after.superblock(), before); EXPECT_EQ(metadata_->findIndexRoot(after, index), root);
    EXPECT_EQ(properties_->readEntity(after, {NODE, 1}), original); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
}
TEST_F(HybridPropertyOrdinaryConversionTest, ConversionWorkspaceObeysTheExistingParentAccountAndRefundsItsCredit) {
    seed(NODE, 2); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    using namespace graph::query::execution::detail;
    for (const auto maximum : {size_t(65536), size_t(1024 * 1024)}) {
        auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
        auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
        auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_WRITE, maximum);
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024, account);
        if (maximum == 65536) EXPECT_THROW(run(request(NODE, memory)), ResourceAdmissionDenied);
        else expectPreparedRefusal(request(NODE, memory));
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(account->used(), 0U);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    }
}
TEST_F(HybridPropertyOrdinaryConversionTest, LargeBackedRowReusesAuthenticatedSingleFieldValueWithoutPublishing) {
    LargeValueStore largeValues(io_, *metadata_);
    PropertyCollectionRootStore roots(*slotted_pages_, largeValues, *metadata_);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, options);
    auto transaction = planner.beginTransaction(); const auto id = transaction.createNode({});
    const PropertyCollectionRoot original{id, graph::Node::typeId, {{1, PropertyValue(std::string(8192, 'x'))}}};
    transaction.replaceNodeProperty(original); ASSERT_TRUE(transaction.commit().committed());
    auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto bytes = io_->fileSize(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    try { run(request(NODE, memory)); FAIL() << "private large field was published"; }
    catch (const FormatException &error) {
        EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
        EXPECT_NE(std::string(error.what()).find("physical publication is not installed"), std::string::npos);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(roots.readEntity(old, {NODE, id}), original);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, id}), original);
}
TEST_F(HybridPropertyOrdinaryConversionTest, WholeRowLargeCarrierBecomesOneSharedPrivateFieldProposal) {
    LargeValueStore largeValues(io_, *metadata_);
    PropertyCollectionRootStore roots(*slotted_pages_, largeValues, *metadata_);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, options);
    auto transaction = planner.beginTransaction();
    for (size_t i = 0; i < 2; ++i) {
        const auto id = transaction.createNode({});
        transaction.replaceNodeProperty({id, graph::Node::typeId,
            {{1, PropertyValue(std::string(8192, 'x'))}, {2, PropertyValue(int64_t(17))}}});
    }
    ASSERT_TRUE(transaction.commit().committed());
    auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto bytes = io_->fileSize(); const auto epoch = io_->dataMutationEpoch();
    struct Stats { int64_t values = -1, fresh = -1; } stats;
    graph::debug::PerfTrace::ScopedObserver observer(&stats,
        [](void *owner, std::string_view key, int64_t value, bool) noexcept {
            auto &s = *static_cast<Stats *>(owner);
            if (key == "native_graph.hybrid.private_large_values") s.values = value;
            if (key == "native_graph.hybrid.private_new_large_values") s.fresh = value;
        });
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    expectPreparedRefusal(request(NODE, memory));
    EXPECT_EQ(stats.values, 1); EXPECT_EQ(stats.fresh, 1);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    for (uint64_t id = 1; id <= 2; ++id) {
        const auto value = roots.readEntity(old, {NODE, id});
        ASSERT_EQ(value.entries.size(), 2U);
        EXPECT_EQ(value.entries[0].value, PropertyValue(std::string(8192, 'x')));
        EXPECT_EQ(value.entries[1].value, PropertyValue(int64_t(17)));
    }
}
#endif
} // namespace
