#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include <gtest/gtest.h>
#include <bit>
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/property/HybridPropertyGroupConversion.hpp"
#include "graph/storage/relocatable/property/HybridPropertyIndexIds.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
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
using HybridPropertyOrdinaryConversionTest = graph::storage::test::HybridPropertyOrdinaryConversionTestFixture;
TEST_F(HybridPropertyOrdinaryConversionTest, FormatFlagControlsTheTypedOrdinaryRequestBeforeAnyPublication) {
    seed(NODE, 2); const auto before = metadata_->openSnapshot().superblock();
    const auto bytes = io_->fileSize(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); const auto conversion = request(NODE, memory);
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) expectPreparedRefusal(conversion);
    else EXPECT_THROW(run(conversion), FormatException);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyOrdinaryConversionTest, NativeTransactionStagesConversionWithoutEnablingItsUninstalledPublication) {
    seed(NODE, 2); const auto before = metadata_->openSnapshot().superblock();
    const auto bytes = io_->fileSize(), epoch = io_->dataMutationEpoch();
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto transaction = planner.beginTransaction();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
        transaction.stageHybridPropertyConversion(NODE, 1, memory);
        EXPECT_EQ(transaction.baseSuperblock(), before);
        EXPECT_EQ(transaction.baseSnapshot().superblock(), before);
        EXPECT_THROW(static_cast<void>(transaction.createNode({})), FormatException);
        EXPECT_THROW(transaction.reserveEdgeIds(1), FormatException);
        EXPECT_THROW(transaction.stageHybridPropertyConversion(NODE, 1, memory), FormatException);
        try { static_cast<void>(transaction.commit()); FAIL() << "incomplete layout was published"; }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("physical publication is not installed"), std::string::npos);
        }
        EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
    } else {
        EXPECT_THROW(transaction.stageHybridPropertyConversion(NODE, 1, memory), FormatException);
        EXPECT_THROW(transaction.stageHybridPropertyColumns({}, memory), FormatException);
        EXPECT_TRUE(transaction.isActive()); transaction.rollback();
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOrdinaryConversionTest, NativeConversionMoveRollbackAndConcurrentPublicationRespectOwnership) {
    seed(NODE, 2);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    {
        auto transaction = planner.beginTransaction();
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        const std::weak_ptr<std::pmr::memory_resource> retained = memory;
        transaction.stageHybridPropertyConversion(NODE, 1, memory); memory.reset();
        auto moved = std::move(transaction); transaction.rollback();
        EXPECT_FALSE(retained.expired());
        auto assigned = planner.beginTransaction(); assigned = std::move(moved); moved.rollback();
        EXPECT_FALSE(retained.expired()); assigned.rollback(); EXPECT_TRUE(retained.expired());
    }
    auto stale = planner.beginTransaction();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    stale.stageHybridPropertyConversion(NODE, 1, memory);
    auto newer = planner.beginTransaction(); const auto created = newer.createNode({});
    ASSERT_TRUE(newer.commit().committed());
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_FALSE(stale.commit().committed());
    EXPECT_EQ(stale.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(entities_->readNode(metadata_->openSnapshot(), created).inline_label_count, 0U);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, NativeConversionRejectsInvalidAccountsAndMixedStagingBeforePublication) {
    seed(NODE, 2); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto transaction = planner.beginTransaction();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    EXPECT_THROW(transaction.stageHybridPropertyConversion(NODE, 0, memory), FormatException);
    EXPECT_THROW(transaction.stageHybridPropertyConversion(EntityPropertyOwnerKind(255), 1, memory), FormatException);
    EXPECT_THROW(transaction.stageHybridPropertyConversion(NODE, 1, {}), FormatException);
    auto oversized = std::make_shared<MaterializedByteBudget>(2 * 1024 * 1024);
    EXPECT_THROW(transaction.stageHybridPropertyConversion(NODE, 1, oversized), FormatException);
    transaction.replaceNodeProperty({1, graph::Node::typeId, {{1, PropertyValue(int64_t(99))}}});
    EXPECT_THROW(transaction.stageHybridPropertyConversion(NODE, 1, memory), FormatException);
    EXPECT_TRUE(transaction.isActive()); transaction.rollback();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, NativeConversionEarlyCancellationRetainsAnUnpublishedActiveSourceForRetry) {
    seed(NODE, 2); const auto before = metadata_->openSnapshot().superblock();
    const auto bytes = io_->fileSize(), epoch = io_->dataMutationEpoch();
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {options.first_slot, options.last_slot, 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    struct ToggleCancellation final : CancellationCheckpoint {
        bool cancelled = true;
        void check() const override { if (cancelled) throw std::runtime_error("cancel before native publication"); }
    } cancellation;
    auto tx = planner.beginTransaction();
    EXPECT_THROW(tx.stageHybridPropertyConversion(NODE, 1, memory, &cancellation), std::runtime_error);
    EXPECT_TRUE(tx.isActive()); EXPECT_EQ(memory->used(), 0U);
    cancellation.cancelled = false;
    EXPECT_NO_THROW(tx.stageHybridPropertyConversion(NODE, 1, memory, &cancellation));
    cancellation.cancelled = true;
    EXPECT_THROW(tx.commit(), std::runtime_error);
    EXPECT_TRUE(tx.isActive()); EXPECT_EQ(tx.baseSuperblock(), before); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->fileSize(), bytes);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    cancellation.cancelled = false;
    // Retry proceeds into the existing private physical preparation; it still
    // cannot report a successful conversion while publication is uninstalled.
    EXPECT_THROW(tx.commit(), FormatException);
    EXPECT_EQ(tx.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
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
    const auto conversion = request(NODE, memory);
    const auto stats = prepareWithStats(conversion);
    EXPECT_GT(stats.fields, 0); EXPECT_GT(stats.indexes, 0);
    if (conversion.conversion->compact_column_memberships) {
        // This dense fixture needs two ROW pools. Direct compact locators
        // need one field pool plus one Group directory pool, without the old
        // OwnerMap/FieldDirectory pools. Equality is not the 10% adoption gate.
        EXPECT_EQ(rowPoolCount, 2U); EXPECT_EQ(stats.indexes, 1);
        EXPECT_LE(stats.fields, int64_t(RSS_PAGE_POOL_DATA_FRAME_COUNT));
        EXPECT_EQ(stats.pool_bytes, int64_t(rowPoolCount * RSS_SEGMENT_SIZE));
    } else EXPECT_GT(stats.pool_bytes, int64_t(rowPoolCount * RSS_SEGMENT_SIZE));
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
    struct Stats { int64_t values = -1, fresh = -1, data = -1, manifests = -1; } stats;
    graph::debug::PerfTrace::ScopedObserver observer(&stats,
        [](void *owner, std::string_view key, int64_t value, bool) noexcept {
            auto &s = *static_cast<Stats *>(owner);
            if (key == "native_graph.hybrid.private_large_values") s.values = value;
            if (key == "native_graph.hybrid.private_new_large_values") s.fresh = value;
            if (key == "native_graph.hybrid.private_large_data_frames") s.data = value;
            if (key == "native_graph.hybrid.private_large_manifest_frames") s.manifests = value;
        });
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    expectPreparedRefusal(request(NODE, memory));
    EXPECT_EQ(stats.values, 1); EXPECT_EQ(stats.fresh, 1);
    EXPECT_GT(stats.data, 0); EXPECT_EQ(stats.manifests, 1);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    for (uint64_t id = 1; id <= 2; ++id) {
        const auto value = roots.readEntity(old, {NODE, id});
        ASSERT_EQ(value.entries.size(), 2U);
        EXPECT_EQ(value.entries[0].value, PropertyValue(std::string(8192, 'x')));
        EXPECT_EQ(value.entries[1].value, PropertyValue(int64_t(17)));
    }
    auto small = std::make_shared<MaterializedByteBudget>(128 * 1024);
    EXPECT_THROW(run(request(NODE, small)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    expectPreparedRefusal(request(NODE, memory));
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, ExplicitCandidateCommitPublishesRealFieldsAndRowAuthorityAtomically) {
    seed(NODE); const auto old = metadata_->openSnapshot();
    const auto original = properties_->readEntity(old, {NODE, 1});
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto tx = planner.beginTransaction();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    tx.stageHybridPropertyConversion(NODE, 1, memory, nullptr, true);
    ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    {
        const auto pin = metadata_->openSnapshot();
        EXPECT_EQ(pin.superblock().generation.value(), old.superblock().generation.value() + 1);
        EXPECT_TRUE(pin.superblock().hybrid_property_descriptor_root.isValid());
        EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(pin, {NODE, 1}));
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}), original);
        EXPECT_EQ(properties_->readEntityPropertyCount(pin, {NODE, 49}), 0U);
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
        EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 96U); EXPECT_EQ(report.columns, 2U);
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    }
    const auto epoch = io_->dataMutationEpoch(), bytes = io_->fileSize();
    // Reclamation and unadapted graph/standalone writes fail at writer entry,
    // before modifying physical data, rather than corrupting a routed group.
    EXPECT_THROW((void)metadata_->upsertCatalog({}, LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 2)), FormatException);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    EXPECT_EQ(memory->used(), 0U);
    auto reopened = RelocatableMetadataStore::openReadOnly(io_);
    SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(pages, *reopened);
    const auto pin = reopened->openSnapshot();
    EXPECT_EQ(roots.readEntity(pin, {NODE, 1}), original);
    EXPECT_EQ(roots.readEntityPropertyCount(pin, {NODE, 49}), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, ExplicitSparseCandidateInstallsExtensionRoutesAndPreservesOldPins) {
    seed(EDGE, 2); const auto old = metadata_->openSnapshot();
    const auto original = properties_->readEntity(old, {EDGE, 1});
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto pin = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntity(pin, {EDGE, 1}), original);
    EXPECT_EQ(properties_->readEntity(old, {EDGE, 1}), original);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 4U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.extensions, 4U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, CandidateABRecoverySelectsMatchingDescriptorMapsAndRowAuthority) {
    seed(NODE, 2); const auto old = metadata_->openSnapshot();
    const auto original = properties_->readEntity(old, {NODE, 1});
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
                                BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        {
            NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
            auto tx = planner.beginTransaction();
            tx.stageHybridPropertyConversion(NODE, 1, std::make_shared<MaterializedByteBudget>(1024 * 1024), nullptr, true);
            MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
            EXPECT_THROW((void)tx.commit(publication), BootstrapPublishInterrupted);
        }
        auto recovered = RelocatableMetadataStore::openReadOnly(io_);
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered);
        const auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root.isValid(), installed);
        EXPECT_EQ(bool(recovered->findEntityPropertyLocationOptional(pin, {NODE, 1})), !installed);
        EXPECT_EQ(roots.readEntity(pin, {NODE, 1}), original);
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    }
}
TEST_F(HybridPropertyOrdinaryConversionTest, CancellationAfterWritingOneCandidatePoolKeepsRowAuthorityAndRetries) {
    seed(NODE, 2); const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    CancelAfterPrivatePreparation cancellation;
    auto conversion = request(NODE, memory);
    conversion.conversion->publish_candidate = true; conversion.conversion->cancellation = &cancellation;
    const auto epoch = io_->dataMutationEpoch();
    {
        graph::debug::PerfTrace::ScopedObserver observe(&cancellation,
            [](void *owner, std::string_view key, int64_t, bool) noexcept {
                if (key == "native_graph.write.new_pool_prepare_calls")
                    static_cast<CancelAfterPrivatePreparation *>(owner)->prepared = true;
            });
        EXPECT_THROW(run(conversion), PrivatePreparationCancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_GT(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    conversion.conversion->cancellation = nullptr;
    EXPECT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), original);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
}
TEST_F(HybridPropertyOrdinaryConversionTest, CandidateReusesIndependentLargeFieldAndExplicitlyReplacesAndRemovesItsReference) {
    LargeValueStore largeValues(io_, *metadata_);
    PropertyCollectionRootStore roots(*slotted_pages_, largeValues, *metadata_);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, options);
    auto tx = planner.beginTransaction(); const auto id = tx.createNode({});
    const PropertyCollectionRoot multiple{id, graph::Node::typeId,
        {{1, PropertyValue(std::string(8192, 'x'))}, {2, PropertyValue(int64_t(17))}}};
    tx.replaceNodeProperty(multiple); ASSERT_TRUE(tx.commit().committed());
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->publish_candidate = false;
    expectPreparedRefusal(conversion);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    const PropertyCollectionRoot single{id, graph::Node::typeId, {{1, PropertyValue(std::string(8192, 'x'))}}};
    auto update = planner.beginTransaction(); update.replaceNodeProperty(single); ASSERT_TRUE(update.commit().committed());
    const auto old = metadata_->openSnapshot();
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    EXPECT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot();
    EXPECT_EQ(roots.readEntity(pin, {NODE, id}), single); EXPECT_EQ(roots.readEntity(old, {NODE, id}), single);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory, &largeValues);
    EXPECT_EQ(report.extensions, 1U); EXPECT_EQ(report.large_values, 1U); EXPECT_EQ(memory->used(), 0U);
    for (const bool erase : {false, true}) {
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, id}, 1, int64_t(7), erase}}};
        const auto state = metadata_->openSnapshot().superblock();
        NativeHybridPropertyColumnWrite write{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
        if (erase) EXPECT_FALSE(roots.readEntityOptional(metadata_->openSnapshot(), {NODE, id}));
        else EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, id}).entries[0].value, PropertyValue(int64_t(7)));
        EXPECT_EQ(roots.readEntity(pin, {NODE, id}), single); EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, CandidateRowPageCowPreservesAnUnconvertedOwnerInTheSameSourcePage) {
    seed(NODE, 65); const auto old = metadata_->openSnapshot();
    const auto converted = properties_->readEntity(old, {NODE, 64});
    const auto unchanged = properties_->readEntity(old, {NODE, 65});
    const auto oldLocation = metadata_->findEntityPropertyLocation(old, {NODE, 65});
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot();
    EXPECT_EQ(metadata_->findEntityPropertyLocation(pin, {NODE, 65}), oldLocation);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 65}), unchanged);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 64}), converted);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 65}), unchanged);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 64}), converted);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, CandidateCanPublishAPropertylessLiveGroupWithoutEmptyFieldTrees) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot();
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    EXPECT_EQ(context.count({NODE, 1}), 0U); EXPECT_EQ(context.count({NODE, 2}), 0U);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 0U); EXPECT_EQ(report.columns, 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, GlobalAuditRejectsChecksummedCandidateNamespaceSubstitutionAndRefunds) {
    seed(NODE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto pin = metadata_->openSnapshot(); const auto &source = pin.superblock();
    MetadataPageFile file(io_, source);
    auto descriptor = file.readHybridPropertyDescriptor(source.hybrid_property_descriptor_root, source);
    const auto original = HybridPropertyDescriptorCodec::encode(descriptor);
    std::swap(descriptor.descriptor.roots[0], descriptor.descriptor.roots[2]);
    const auto damaged = HybridPropertyDescriptorCodec::encode(descriptor);
    const auto reference = source.hybrid_property_descriptor_root;
    const BufferPoolPageKey key{file.pageOffset(reference), reference.generation.value()};
    io_->writeAt(key, damaged.data(), damaged.size());
    EXPECT_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan(), FormatException);
    EXPECT_EQ(memory->used(), 0U);
    io_->writeAt(key, original.data(), original.size());
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, DurableInverseRestoresRowWritesAndRetiresOnlyTheCandidatePools) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto original = properties_->readEntity(metadata_->openSnapshot(), {NODE, 1});
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); auto hybrid = metadata_->openSnapshot();
    const auto nativeGeneration = hybrid.superblock().generation;
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    std::vector<CatalogSegmentRetirementRequest> retired;
    {
        const auto pin = metadata_->openSnapshot(); EXPECT_FALSE(pin.superblock().hybrid_property_descriptor_root.isValid());
        EXPECT_EQ(pin.superblock().generation.value(), nativeGeneration.value() + 1);
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}), original);
        EXPECT_EQ(properties_->readEntityPropertyCount(pin, {NODE, 49}).value_or(0), 0U);
        EXPECT_EQ(properties_->readEntity(hybrid, {NODE, 1}), original);
        metadata_->visitRetireQueueEntries(pin, [&](const auto &entry) {
            const auto pool = metadata_->findCatalog(pin, entry.segment_id);
            EXPECT_EQ(pool.state, SegmentState::SGS_RETIRED);
            EXPECT_LE(entry.retired_generation, pin.superblock().generation);
            retired.push_back({pool.segment_id, pool.generation});
        });
        ASSERT_GE(retired.size(), 3U);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
    auto now = metadata_->openSnapshot().superblock();
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(now.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(properties_->readEntity(hybrid, {NODE, 1}), original);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto write = planner.beginTransaction();
    const PropertyCollectionRoot changed{1, graph::Node::typeId, {{1, PropertyValue(int64_t(999))}}};
    write.replaceNodeProperty(changed); ASSERT_TRUE(write.commit().committed());
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), changed);
    EXPECT_EQ(properties_->readEntity(hybrid, {NODE, 1}), original);
    hybrid.release(); now = metadata_->openSnapshot().superblock();
    EXPECT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(now.checkpoint_lsn.value() + 1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    reopenNativeStores(); const auto reopened = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntity(reopened, {NODE, 1}), changed);
}
TEST_F(HybridPropertyOrdinaryConversionTest, NativeInverseStageAndRecoveryKeepOneCompleteLayout) {
    seed(EDGE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto original = properties_->readEntity(metadata_->openSnapshot(), {EDGE, 1});
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto hybrid = metadata_->openSnapshot();
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
                                BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        {
            auto tx = planner.beginTransaction();
            EXPECT_THROW(tx.stageHybridPropertyConversion(EDGE, 1, memory, nullptr, true,
                NativeHybridPropertyConversionDirection(255)), FormatException);
            EXPECT_TRUE(tx.isActive());
            tx.stageHybridPropertyConversion(EDGE, 1, memory, nullptr, true,
                NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW);
            MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
            EXPECT_THROW((void)tx.commit(publication), BootstrapPublishInterrupted); EXPECT_EQ(memory->used(), 0U);
        }
        auto recovered = RelocatableMetadataStore::openReadOnly(io_);
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        const bool row = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root.isValid(), !row);
        EXPECT_EQ(bool(recovered->findEntityPropertyLocationOptional(pin, {EDGE, 1})), row);
        EXPECT_EQ(roots.readEntity(pin, {EDGE, 1}), original);
        EXPECT_EQ(properties_->readEntity(hybrid, {EDGE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyOrdinaryConversionTest, InverseCancellationAndQuotaRefundKeepTheDurableHybridSourceForRetry) {
    seed(NODE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto small = std::make_shared<MaterializedByteBudget>(32768);
    conversion = request(NODE, small); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    EXPECT_THROW(run(conversion), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    CancelAfterPrivatePreparation cancellation; conversion.conversion->cancellation = &cancellation;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&cancellation,
            [](void *owner, std::string_view key, int64_t, bool) noexcept {
                if (key == "native_graph.hybrid.inverse_private_row_frames")
                    static_cast<CancelAfterPrivatePreparation *>(owner)->prepared = true;
            });
        EXPECT_THROW(run(conversion), PrivatePreparationCancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    conversion.conversion->cancellation = nullptr; EXPECT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    EXPECT_FALSE(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root.isValid());
}
TEST_F(HybridPropertyOrdinaryConversionTest, InversePoolReleaseAndReuseCannotAliasAnOldNativeLocator) {
    seed(NODE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    auto old = metadata_->openSnapshot();
    HybridPropertyReadContext oldContext(io_, *slotted_pages_, *metadata_, old);
    std::vector<CatalogSegmentRetirementRequest> retired;
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion));
    PhysicalSlotId reusedSlot; PageId oldField;
    {
        const auto pin = metadata_->openSnapshot();
        metadata_->visitRetireQueueEntries(pin, [&](const auto &entry) {
            const auto pool = metadata_->findCatalog(pin, entry.segment_id);
            retired.push_back({pool.segment_id, pool.generation});
            if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL) reusedSlot = pool.physical_slot;
        });
        static_cast<void>(metadata_->visitPageLocationsUntil(old, [&](const auto &location) {
            const auto pool = metadata_->findCatalog(old, location.segment_id);
            if (pool.physical_slot == reusedSlot) oldField = location.page_id;
            return true;
        }));
    }
    ASSERT_TRUE(reusedSlot.isValid()); ASSERT_TRUE(oldField.isValid());
    auto current = metadata_->openSnapshot().superblock();
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(current.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(oldContext.count({NODE, 1}), 2U);
    old.release();
    ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(current.checkpoint_lsn.value() + 1)));
    current = metadata_->openSnapshot().superblock();
    SlottedPagePoolAllocationRequest allocation;
    allocation.segment_id = SegmentId(current.next_logical_segment_id); allocation.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
    allocation.pool_owner_id = allocation.page_owner_id = 777; allocation.page_owner_kind = PageOwnerKind::POK_PROPERTY;
    allocation.generation = Generation(2); allocation.page_id = PageId(current.next_logical_page_id);
    const auto reused = slotted_pages_->allocateEmptyPage(allocation, reusedSlot, reusedSlot,
        LogSequenceNumber(current.checkpoint_lsn.value() + 1));
    ASSERT_TRUE(reused); EXPECT_EQ(reused->allocation.pool.physical_slot, reusedSlot);
    const auto pin = metadata_->openSnapshot(); EXPECT_NE(allocation.page_id, oldField);
    EXPECT_FALSE(metadata_->findPageLocationOptional(pin, oldField));
    EXPECT_EQ(slotted_pages_->read(pin, allocation.page_id).pageGeneration(), Generation(2));
    EXPECT_THROW((void)oldContext.count({NODE, 1}), FormatException);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, InverseReusesTheIndependentLargeFieldCarrierWithoutCreatingAValue) {
    LargeValueStore largeValues(io_, *metadata_);
    PropertyCollectionRootStore roots(*slotted_pages_, largeValues, *metadata_);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, options);
    auto tx = planner.beginTransaction(); const auto id = tx.createNode({});
    const PropertyCollectionRoot expected{id, graph::Node::typeId, {{1, PropertyValue(std::string(8192, 'v'))}}};
    tx.replaceNodeProperty(expected); ASSERT_TRUE(tx.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto hybrid = metadata_->openSnapshot(); const auto nextValue = hybrid.superblock().next_large_value_id;
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot(); EXPECT_FALSE(pin.superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_EQ(pin.superblock().next_large_value_id, nextValue);
    EXPECT_EQ(roots.readEntity(pin, {NODE, id}), expected); EXPECT_EQ(roots.readEntity(hybrid, {NODE, id}), expected);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, PropertylessGroupCanReturnToRowAndCommitOrdinaryWrites) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot(); EXPECT_FALSE(pin.superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_FALSE(properties_->readEntityOptional(pin, {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto write = planner.beginTransaction(); write.replaceNodeProperty({1, graph::Node::typeId, {{7, PropertyValue(true)}}});
    EXPECT_TRUE(write.commit().committed()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, InverseCancellationAfterPhysicalWritesNeverPublishesHalfARowLayout) {
    seed(NODE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    CancelAfterPrivatePreparation cancellation; conversion.conversion->cancellation = &cancellation;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&cancellation,
            [](void *owner, std::string_view key, int64_t, bool) noexcept {
                if (key == "native_graph.write.new_pool_prepare_calls")
                    static_cast<CancelAfterPrivatePreparation *>(owner)->prepared = true;
            });
        EXPECT_THROW(run(conversion), PrivatePreparationCancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_GT(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    conversion.conversion->cancellation = nullptr; EXPECT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    EXPECT_FALSE(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyOrdinaryConversionTest, InverseCreatesANewOversizedRowCarrierAfterItsFieldsChange) {
    LargeValueStore largeValues(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, largeValues, *metadata_);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, options);
    auto tx = planner.beginTransaction(); const auto id = tx.createNode({});
    PropertyCollectionRoot expected{id, graph::Node::typeId,
        {{1, PropertyValue(std::string(2000, 'a'))}, {2, PropertyValue(std::string(2000, 'b'))}}};
    tx.replaceNodeProperty(expected); ASSERT_TRUE(tx.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    // Changing one inline field removes the exact whole-ROW dedup candidate;
    // inverse conversion must publish a fresh oversized carrier atomically.
    const auto native = metadata_->openSnapshot().superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, id}, 1,
        graph::OwnedPropertyValue(PropertyValue(std::string(2000, 'c')))}}};
    NativeHybridPropertyColumnWrite write{native.database_id, native.generation, native.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(native.checkpoint_lsn.value() + 1)));
    expected.entries[0].value = PropertyValue(std::string(2000, 'c'));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto epoch = io_->dataMutationEpoch();
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion));
    const auto after = metadata_->openSnapshot().superblock();
    EXPECT_EQ(after.next_large_value_id, before.next_large_value_id + 1U);
    EXPECT_EQ(after.generation.value(), before.generation.value() + 1U);
    EXPECT_FALSE(after.hybrid_property_descriptor_root.isValid()); EXPECT_GT(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(roots.readEntity(old, {NODE, id}), expected); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, id}), expected);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, DurableRowHybridRowRoundTripPreservesNullSignedZeroAndNaNPayload) {
    seed(NODE); NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto update = planner.beginTransaction();
    constexpr uint64_t nanBits = UINT64_C(0x7ff80000abc12345);
    for (uint64_t id = 1; id <= 48; ++id) {
        PropertyValue value(id == 1 ? -0.0 : id == 2 ? std::bit_cast<double>(nanBits) : double(id) + 0.25);
        if (id == 3) value = PropertyValue();
        update.replaceNodeProperty({id, graph::Node::typeId, {{1, value}}});
    }
    ASSERT_TRUE(update.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto hybrid = metadata_->openSnapshot();
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot();
    for (const auto *source : {&pin, &hybrid}) {
        const auto zero = properties_->readEntity(*source, {NODE, 1});
        const auto nan = properties_->readEntity(*source, {NODE, 2});
        const auto null = properties_->readEntity(*source, {NODE, 3});
        EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(zero.entries[0].value.getVariant())), UINT64_C(1) << 63);
        EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(nan.entries[0].value.getVariant())), nanBits);
        EXPECT_TRUE(std::holds_alternative<std::monostate>(null.entries[0].value.getVariant()));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, MaximumBoundedGroupCommitsBothDirectionsWithinOneMiB) {
    seed(NODE, 64, 64); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto expected = properties_->readEntity(metadata_->openSnapshot(), {NODE, 64});
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    const auto hybrid = metadata_->openSnapshot();
    EXPECT_EQ(properties_->readEntity(hybrid, {NODE, 64}), expected);
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    const auto row = metadata_->openSnapshot(); EXPECT_FALSE(row.superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_EQ(properties_->readEntity(row, {NODE, 64}), expected); EXPECT_EQ(properties_->readEntity(hybrid, {NODE, 64}), expected);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
} // namespace

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyOrdinaryConversionTest, ForwardDropsEmptyRowMappingsAndQueuesTheDetachedPoolWithoutTouchingOldPins) {
    seed(NODE); auto old = metadata_->openSnapshot();
    const auto root = metadata_->findEntityPropertyLocation(old, {NODE, 1});
    const auto location = metadata_->findPageLocation(old, root.page_id);
    const auto pool = metadata_->findCatalog(old, location.segment_id);
    const auto value = properties_->readEntity(old, {NODE, 1});
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    {
        const auto pin = metadata_->openSnapshot();
        EXPECT_FALSE(metadata_->findPageLocationOptional(pin, root.page_id));
        EXPECT_EQ(metadata_->findCatalog(pin, pool.segment_id).state, SegmentState::SGS_RETIRED);
        bool queued = false;
        metadata_->visitRetireQueueEntries(pin, [&](const auto &entry) {
            if (entry.segment_id == pool.segment_id) {
                queued = true; EXPECT_EQ(entry.retired_generation, pin.superblock().generation);
                EXPECT_EQ(entry.physical_slot, pool.physical_slot);
            }
        });
        EXPECT_TRUE(queued); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), value);
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}), value);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    }
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion));
    const auto current = metadata_->openSnapshot().superblock();
    const std::array<CatalogSegmentRetirementRequest, 1> retired{{{pool.segment_id, pool.generation}}};
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(current.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), value); old.release();
    EXPECT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(current.checkpoint_lsn.value() + 1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, ForwardCannotRetireASourcePoolContainingAnUntouchedCurrentPage) {
    seed(NODE, 129, 8); const auto old = metadata_->openSnapshot();
    const auto first = metadata_->findEntityPropertyLocation(old, {NODE, 1});
    const auto last = metadata_->findEntityPropertyLocation(old, {NODE, 129});
    const auto firstPage = metadata_->findPageLocation(old, first.page_id);
    const auto lastPage = metadata_->findPageLocation(old, last.page_id);
    ASSERT_NE(first.page_id, last.page_id); ASSERT_EQ(firstPage.segment_id, lastPage.segment_id);
    const auto untouched = properties_->readEntity(old, {NODE, 129});
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot();
    EXPECT_FALSE(metadata_->findPageLocationOptional(pin, first.page_id));
    EXPECT_EQ(metadata_->findPageLocation(pin, last.page_id), lastPage);
    EXPECT_EQ(metadata_->findCatalog(pin, firstPage.segment_id).state, SegmentState::SGS_SEALED);
    metadata_->visitRetireQueueEntries(pin, [&](const auto &entry) { EXPECT_NE(entry.segment_id, firstPage.segment_id); });
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 129}), untouched);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 129}), untouched);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}


TEST_F(HybridPropertyOrdinaryConversionTest, DurableColumnChangesKeepDirectoryRootsAndOldSnapshotsWithoutRebuildingOtherFields) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const auto descriptor = MetadataPageFile(io_, before).readHybridPropertyDescriptor(
        before.hybrid_property_descriptor_root, before).descriptor;
    const std::array<NativeHybridPropertyColumnReplacement, 3> cells{{
        {{NODE, 1}, 1, int64_t(999)}, {{NODE, 2}, 1, int64_t(777)}, {{NODE, 3}, 2, std::monostate{}}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(memory->used(), 0U);
    {
        const auto pin = metadata_->openSnapshot(); const auto now = pin.superblock();
        EXPECT_EQ(now.generation.value(), before.generation.value() + 1);
        const auto after = MetadataPageFile(io_, now).readHybridPropertyDescriptor(now.hybrid_property_descriptor_root, now).descriptor;
        EXPECT_EQ(after.roots, descriptor.roots); EXPECT_EQ(after.semantic_generation, now.generation);
        auto expected = original; expected.entries[0].value = PropertyValue(int64_t(999));
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}), expected);
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 2}).entries[0].value, PropertyValue(int64_t(777)));
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 3}).entries[1].value, PropertyValue());
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(now.checkpoint_lsn.value() + 1)), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U);
    auto reopened = RelocatableMetadataStore::openReadOnly(io_);
    SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(pages, *reopened);
    EXPECT_EQ(roots.readEntity(reopened->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(999)));
}
TEST_F(HybridPropertyOrdinaryConversionTest, NativeColumnNoopAndInvalidOrOverlappingWritesLeaveTheSourceUnchanged) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(1)}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903};
    EXPECT_EQ(metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    cells[0].token = 999;
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    cells[0].token = 1; cells[0].value = true;
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    const std::array<NativeHybridPropertyColumnReplacement, 2> duplicates{{{{NODE, 1}, 1, int64_t(2)}, {{NODE, 1}, 1, int64_t(3)}}};
    write.replacements = duplicates;
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyOrdinaryConversionTest, NativeColumnQuotaAndPostFrameCancellationRefundAndRetryWithoutHalfPublishing) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(999)}}};
    auto small = std::make_shared<MaterializedByteBudget>(1024);
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, small, PhysicalSlotId(40), PhysicalSlotId(80), 903};
    const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)),
        graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    write.memory = memory; CancelAfterPrivatePreparation cancellation; write.cancellation = &cancellation;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&cancellation,
            [](void *owner, std::string_view key, int64_t, bool) noexcept {
                if (key == "native_graph.hybrid.pool_frames_prepared")
                    static_cast<CancelAfterPrivatePreparation *>(owner)->prepared = true;
            });
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)),
            PrivatePreparationCancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_GT(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    write.cancellation = nullptr;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(999)));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}

TEST_F(HybridPropertyOrdinaryConversionTest, NativeColumnABRecoverySelectsCompleteNewCellsOrTheCompleteSource) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, int64_t(999)}, {{NODE, 2}, 2, int64_t(777)}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903};
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
                                BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1), publication),
            BootstrapPublishInterrupted);
        EXPECT_EQ(memory->used(), 0U);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_);
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().generation.value(), before.generation.value() + size_t(installed));
        EXPECT_EQ(roots.readEntity(pin, {NODE, 1}).entries[0].value, installed ? PropertyValue(int64_t(999)) : original.entries[0].value);
        EXPECT_EQ(roots.readEntity(pin, {NODE, 2}).entries[1].value, PropertyValue(int64_t(installed ? 777 : 3)));
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}

TEST_F(HybridPropertyOrdinaryConversionTest, RepeatedConversionAndColumnWritesRetireAndReusePoolsAfterSnapshotDrain) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    std::vector<PhysicalSlotId> previousRetired; bool reused = false;
    for (int64_t round = 1; round <= 3; ++round) {
        auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
        const auto native = metadata_->openSnapshot().superblock();
        {
            const auto pin = metadata_->openSnapshot();
            static_cast<void>(metadata_->visitPageLocationsUntil(pin, [&](const auto &location) {
                const auto pool = metadata_->findCatalog(pin, location.segment_id);
                if (std::find(previousRetired.begin(), previousRetired.end(), pool.physical_slot) != previousRetired.end()) reused = true;
                return true;
            }));
        }
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, round * 100}}};
        NativeHybridPropertyColumnWrite write{native.database_id, native.generation, native.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903};
        ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(native.checkpoint_lsn.value() + 1)));
        conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
        conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
        ASSERT_NO_THROW(run(conversion));
        std::vector<CatalogSegmentRetirementRequest> retirements;
        {
            const auto pin = metadata_->openSnapshot();
            EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue(round * 100));
            metadata_->visitRetireQueueEntries(pin, [&](const auto &entry) {
                const auto pool = metadata_->findCatalog(pin, entry.segment_id);
                retirements.push_back({pool.segment_id, pool.generation}); previousRetired.push_back(pool.physical_slot);
            });
        }
        ASSERT_FALSE(retirements.empty()); const auto now = metadata_->openSnapshot().superblock();
        EXPECT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retirements, LogSequenceNumber(now.checkpoint_lsn.value() + 1)));
        const auto pin = metadata_->openSnapshot();
        metadata_->visitRetireQueueEntries(pin, [&](const auto &) { ADD_FAILURE() << "drained conversion left retired data pools"; });
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_TRUE(reused);
}

TEST_F(HybridPropertyOrdinaryConversionTest, NativeColumnTransactionOwnsMovedCellsAndConflictsWithoutLeakingTheAccount) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    const auto old = metadata_->openSnapshot(); const auto original = properties_->readEntity(old, {NODE, 1});
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(999)}}};
    auto first = planner.beginTransaction(); auto second = planner.beginTransaction();
    first.stageHybridPropertyColumns(cells, memory); second.stageHybridPropertyColumns(cells, memory);
    EXPECT_GT(memory->used(), 0U); cells[0].value = int64_t(555);
    NativeGraphWriteTransaction moved(std::move(first)); EXPECT_FALSE(first.isActive());
    EXPECT_EQ(moved.baseSnapshot().superblock(), old.superblock());
    EXPECT_THROW(moved.reserveNodeIds(1), FormatException);
    EXPECT_THROW(moved.replaceNodeProperty(original), FormatException);
    EXPECT_THROW(moved.stageHybridPropertyConversion(NODE, 1, memory), FormatException);
    ASSERT_TRUE(moved.commit().committed());
    EXPECT_FALSE(second.commit().committed()); EXPECT_EQ(second.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(999)));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyOrdinaryConversionTest, NativeColumnStagingFailureRollbackAndEarlyCancellationRetainTheirContracts) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(999)}}};
    {
        auto tx = planner.beginTransaction(); auto tiny = std::make_shared<MaterializedByteBudget>(1);
        EXPECT_THROW(tx.stageHybridPropertyColumns(cells, tiny), graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(tiny->used(), 0U); EXPECT_TRUE(tx.isActive());
        tx.stageHybridPropertyColumns(cells, memory); EXPECT_GT(memory->used(), 0U);
        tx.rollback(); EXPECT_EQ(memory->used(), 0U);
    }
    auto tx = planner.beginTransaction(); CancelAfterPrivatePreparation cancellation;
    tx.stageHybridPropertyColumns(cells, memory, &cancellation); const auto staged = memory->used();
    cancellation.prepared = true;
    EXPECT_THROW((void)tx.commit(), PrivatePreparationCancelled); EXPECT_TRUE(tx.isActive());
    EXPECT_EQ(memory->used(), staged); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    cancellation.prepared = false; ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(999)));
}

TEST_F(HybridPropertyOrdinaryConversionTest, ReopenedWritableMetadataContinuesControlledColumnsAndInverseWithoutUnadaptedReconciliation) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    auto reopened = RelocatableMetadataStore::open(io_);
    EntityPageStore entities(io_, *reopened, SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
    SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
    BTreePageStore indexes(pages, *reopened);
    EntityLabelSetStore labels(io_, *reopened, pages, SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
    PropertyCollectionRootStore roots(pages, *reopened);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*reopened, entities, labels, roots, indexes, options);
    const auto original = roots.readEntity(reopened->openSnapshot(), {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(999)}}};
    {
        auto tx = planner.beginTransaction(); tx.stageHybridPropertyColumns(cells, memory); ASSERT_TRUE(tx.commit().committed());
        EXPECT_EQ(memory->used(), 0U);
    }
    {
        auto reopenedAgain = RelocatableMetadataStore::openReadOnly(io_);
        SlottedPagePool readPages(io_, *reopenedAgain, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore readRoots(readPages, *reopenedAgain);
        EXPECT_EQ(readRoots.readEntity(reopenedAgain->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(999)));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopenedAgain, memory).scan());
    }
    {
        auto tx = planner.beginTransaction(); tx.stageHybridPropertyConversion(NODE, 1, memory, nullptr, true,
            NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW); ASSERT_TRUE(tx.commit().committed());
    }
    SlottedPagePool reconciled(io_, *reopened);
    EntityPageStore rowEntities(io_, *reopened);
    BTreePageStore rowIndexes(reconciled, *reopened);
    EntityLabelSetStore rowLabels(io_, *reopened, reconciled);
    PropertyCollectionRootStore rowRoots(reconciled, *reopened);
    NativeGraphWritePlanner rowPlanner(*reopened, rowEntities, rowLabels, rowRoots, rowIndexes, options);
    auto tx = rowPlanner.beginTransaction();
    auto changed = original; changed.entries[0].value = PropertyValue(int64_t(555));
    tx.replaceNodeProperty(changed); ASSERT_TRUE(tx.commit().committed());
    EXPECT_EQ(rowRoots.readEntity(reopened->openSnapshot(), {NODE, 1}), changed);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyOrdinaryConversionTest, ActiveNativeColumnsReleaseDetachedPoolsAndReuseSlotsWithoutInverseOrCatalogHistoryGrowth) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    std::vector<PhysicalSlotId> freedSlots; bool reused = false;
    const auto collect = [&] {
        std::vector<CatalogSegmentRetirementRequest> requests; const auto pin = metadata_->openSnapshot();
        metadata_->visitRetireQueueEntries(pin, [&](const auto &entry) {
            const auto pool = metadata_->findCatalog(pin, entry.segment_id); requests.push_back({pool.segment_id, pool.generation});
            freedSlots.push_back(pool.physical_slot);
        }); return requests;
    };
    auto retired = collect(); ASSERT_FALSE(retired.empty());
    auto now = metadata_->openSnapshot().superblock();
    ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(now.checkpoint_lsn.value() + 1)));
    for (int64_t round = 1; round <= 5; ++round) {
        auto old = metadata_->openSnapshot(); const auto before = old.superblock();
        const auto original = properties_->readEntity(old, {NODE, 1});
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, round * 100}}};
        NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903};
        ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
        // Explicit maintenance, rather than each small write, detaches a pool.
        {
            const auto pin = metadata_->openSnapshot(); SegmentId sourcePool;
            static_cast<void>(metadata_->visitPageLocationsUntil(pin, [&](const PageLocation &route) {
                const auto pool = metadata_->findCatalog(pin, route.segment_id);
                if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && pool.owner_id == 903) { sourcePool = pool.segment_id; return false; }
                return true;
            }));
            ASSERT_TRUE(sourcePool.isValid()); const auto &state = pin.superblock();
            NativeHybridPropertyPoolRelocation move{state.database_id, state.generation, state.hybrid_property_descriptor_root,
                sourcePool, PhysicalSlotId(40), PhysicalSlotId(80), memory};
            ASSERT_NO_THROW((void)metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
        }
        retired = collect(); ASSERT_FALSE(retired.empty()); now = metadata_->openSnapshot().superblock();
        const auto epoch = io_->dataMutationEpoch();
        EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(now.checkpoint_lsn.value() + 1)));
        EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original); old.release();
        ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(now.checkpoint_lsn.value() + 1)));
        const auto pin = metadata_->openSnapshot();
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root, now.hybrid_property_descriptor_root);
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue(round * 100));
        for (const auto &entry : retired) EXPECT_THROW((void)metadata_->findCatalog(pin, entry.segment_id), FormatException);
        metadata_->visitRetireQueueEntries(pin, [&](const auto &) { ADD_FAILURE() << "active native release left queued pools"; });
        static_cast<void>(metadata_->visitPageLocationsUntil(pin, [&](const auto &location) {
            const auto pool = metadata_->findCatalog(pin, location.segment_id);
            if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL &&
                std::find(freedSlots.begin(), freedSlots.end(), pool.physical_slot) != freedSlots.end()) reused = true;
            return true;
        }));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_TRUE(reused);
}
TEST_F(HybridPropertyOrdinaryConversionTest, ActiveNativePoolReleaseABRecoveryPreservesCellsAndOneCompleteAllocatorQueueState) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    std::vector<CatalogSegmentRetirementRequest> retired;
    {
        const auto pin = metadata_->openSnapshot(); metadata_->visitRetireQueueEntries(pin, [&](const auto &entry) {
            const auto pool = metadata_->findCatalog(pin, entry.segment_id); retired.push_back({pool.segment_id, pool.generation});
        });
    }
    ASSERT_FALSE(retired.empty()); const auto before = metadata_->openSnapshot().superblock();
    const auto original = properties_->readEntity(metadata_->openSnapshot(), {NODE, 1});
    const std::vector<CatalogSegmentRetirementRequest> overBound(65, retired.front());
    EXPECT_THROW((void)metadata_->tryReleaseRetiredCatalogSegments(overBound, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
                                BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW((void)metadata_->tryReleaseRetiredCatalogSegments(retired, LogSequenceNumber(before.checkpoint_lsn.value() + 1), publication),
            BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_);
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        const bool released = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        for (const auto &entry : retired) {
            if (released) EXPECT_THROW((void)recovered->findCatalog(pin, entry.segment_id), FormatException);
            else EXPECT_NO_THROW((void)recovered->findCatalog(pin, entry.segment_id));
        }
        EXPECT_EQ(roots.readEntity(pin, {NODE, 1}), original);
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan());
    }
}

TEST_F(HybridPropertyOrdinaryConversionTest, ActiveNativePoolReleaseRejectsStaleDuplicateAndDamagedSourceIdentitiesBeforePublishing) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    std::vector<CatalogSegmentRetirementRequest> retired; CatalogEntry pool;
    {
        const auto pin = metadata_->openSnapshot(); metadata_->visitRetireQueueEntries(pin, [&](const auto &entry) {
            pool = metadata_->findCatalog(pin, entry.segment_id); retired.push_back({pool.segment_id, pool.generation});
        });
    }
    ASSERT_EQ(retired.size(), 1U); const auto before = metadata_->openSnapshot().superblock();
    const auto originalValue = properties_->readEntity(metadata_->openSnapshot(), {NODE, 1});
    const auto checkpoint = LogSequenceNumber(before.checkpoint_lsn.value() + 1);
    auto stale = retired; stale[0].expected_generation = Generation(stale[0].expected_generation.value() + 1);
    const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW((void)metadata_->tryReleaseRetiredCatalogSegments(stale, checkpoint), FormatException);
    const std::array<CatalogSegmentRetirementRequest, 2> duplicate{retired[0], retired[0]};
    EXPECT_THROW((void)metadata_->tryReleaseRetiredCatalogSegments(duplicate, checkpoint), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    const BufferPoolPageKey key{PhysicalSegmentAllocator::checkedSlotOffset(before.data_region_offset, pool.physical_slot), pool.generation.value()};
    std::array<std::byte, RSS_PAGE_SIZE> original{};
    ASSERT_EQ(io_->readAt(key, original.data(), original.size()), original.size());
    auto damaged = original; damaged[0] ^= std::byte{1}; io_->writeAt(key, damaged.data(), damaged.size());
    const auto damagedEpoch = io_->dataMutationEpoch();
    EXPECT_THROW((void)metadata_->tryReleaseRetiredCatalogSegments(retired, checkpoint), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), damagedEpoch);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), originalValue);
    io_->writeAt(key, original.data(), original.size());
    EXPECT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(retired, checkpoint));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyOrdinaryConversionTest, DurableFieldRemovalPublishesDemotionDirectoriesAndMixedColumnWritesTogether) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 1, std::monostate{}, true}, {{NODE, 2}, 2, int64_t(999)}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot(); auto expected = original; expected.entries.erase(expected.entries.begin());
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}), expected);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 2}).entries[1].value, PropertyValue(int64_t(999)));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 95U); EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 47U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, TwoOwnerRemovalsMergeBeforeThresholdAndOnlyLaterDemoteTheColumn) {
    seed(NODE, 50); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const std::array<NativeHybridPropertyColumnReplacement, 2> removals{{
        {{NODE, 1}, 1, std::monostate{}, true}, {{NODE, 2}, 1, std::monostate{}, true}}};
    auto before = metadata_->openSnapshot().superblock();
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        removals, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    {
        const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
        EXPECT_EQ(report.memberships, 98U); EXPECT_EQ(report.columns, 2U); EXPECT_EQ(report.extensions, 0U);
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries.size(), 1U);
    }
    const std::array<NativeHybridPropertyColumnReplacement, 1> removal{{{{NODE, 3}, 1, std::monostate{}, true}}};
    before = metadata_->openSnapshot().superblock(); write.expected_generation = before.generation;
    write.expected_descriptor = before.hybrid_property_descriptor_root; write.replacements = removal;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 97U); EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 47U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, NativeTransactionTypeChangeDemotesTheColumnAndSurvivesInverse) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto original = properties_->readEntity(old, {NODE, 1});
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, true}}};
    auto tx = planner.beginTransaction(); tx.stageHybridPropertyColumns(cells, memory, nullptr, true); ASSERT_TRUE(tx.commit().committed());
    {
        const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
        EXPECT_EQ(report.memberships, 96U); EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 48U);
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue(true));
        EXPECT_EQ(properties_->readEntity(pin, {NODE, 2}).entries[0].value, PropertyValue(int64_t(2)));
    }
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion));
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(true));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, FieldRemovalChangesOnlyItsActualFieldPagesAndDirectoryPath) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    std::vector<PageLocation> fieldPages, ownerPages;
    static_cast<void>(metadata_->visitPageLocationsUntil(old, [&](const auto &location) {
        const auto pool = metadata_->findCatalog(old, location.segment_id);
        if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && pool.owner_id == 903) fieldPages.push_back(location);
        if (pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL && pool.owner_id == hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_OWNER)) ownerPages.push_back(location);
        return true;
    }));
    const bool compact = conversion.conversion->compact_column_memberships;
    ASSERT_GE(fieldPages.size(), 3U);
    if (compact) EXPECT_TRUE(ownerPages.empty()); else ASSERT_GE(ownerPages.size(), 3U);
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, std::monostate{}, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); size_t changedFields = 0, unchangedFields = 0, unchangedOwnerPages = 0;
    for (const auto &location : fieldPages) {
        const auto now = metadata_->findPageLocationOptional(pin, location.page_id);
        if (now == std::optional(location)) ++unchangedFields; else ++changedFields;
    }
    for (const auto &location : ownerPages)
        unchangedOwnerPages += metadata_->findPageLocationOptional(pin, location.page_id) == std::optional(location);
    EXPECT_LE(changedFields, 2U); EXPECT_GT(unchangedFields, 0U);
    if (compact) EXPECT_EQ(unchangedOwnerPages, 0U); else EXPECT_GT(unchangedOwnerPages, 0U);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries.size(), 15U);
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}).entries.size(), 16U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyOrdinaryConversionTest, RemovingEveryColumnMemberLeavesOnlyTheLiveGroupAndCanReturnToRow) {
    seed(NODE, 49, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    std::vector<NativeHybridPropertyColumnReplacement> cells;
    for (uint64_t id = 1; id <= 48; ++id) cells.push_back({{NODE, id}, 1, std::monostate{}, true});
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    {
        const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
        EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 0U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.extensions, 0U);
        const auto descriptor = MetadataPageFile(io_, pin.superblock()).readHybridPropertyDescriptor(pin.superblock().hybrid_property_descriptor_root, pin.superblock()).descriptor;
        for (size_t role = 0; role < 3; ++role) EXPECT_FALSE(descriptor.roots[role].page_id.isValid());
        EXPECT_TRUE(descriptor.roots[3].page_id.isValid());
        for (uint64_t id = 1; id <= 49; ++id) EXPECT_FALSE(properties_->readEntityOptional(pin, {NODE, id}));
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}).entries[0].value, PropertyValue(int64_t(1)));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    }
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(memory->used(), 0U);
    EXPECT_FALSE(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_FALSE(properties_->readEntityOptional(metadata_->openSnapshot(), {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyOrdinaryConversionTest, FieldDemotionABRecoverySelectsOneCompleteMapAndFieldGeneration) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 1, std::monostate{}, true}, {{NODE, 2}, 2, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
                                BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1), publication), BootstrapPublishInterrupted);
        EXPECT_EQ(memory->used(), 0U);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_);
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().generation.value(), before.generation.value() + size_t(installed));
        auto expected = original; if (installed) expected.entries.erase(expected.entries.begin());
        EXPECT_EQ(roots.readEntity(pin, {NODE, 1}), expected);
        EXPECT_EQ(roots.readEntity(pin, {NODE, 2}).entries[1].value, installed ? PropertyValue(true) : PropertyValue(int64_t(3)));
        HybridPropertyReadContext context(io_, pages, *recovered, pin);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
        EXPECT_EQ(report.memberships, installed ? 95U : 96U); EXPECT_EQ(report.columns, installed ? 0U : 2U);
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyOrdinaryConversionTest, FieldMapPlanningQuotaAndBothCancellationBoundariesRefundForRetry) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, std::monostate{}, true}}};
    auto small = std::make_shared<MaterializedByteBudget>(128 * 1024);
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, small, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    const auto epoch = io_->dataMutationEpoch();
    // A compact map can legitimately fit the old128KiB fixture cap. Keep
    // that cap and exhaust its remaining credit at the real map boundary,
    // before physical publication; the legacy path may already refuse sooner.
    graph::utils::OwnedHeapReservation pressure(small);
    struct QuotaState { graph::utils::OwnedHeapReservation *pressure; MaterializedByteBudget *memory; bool injected = false, unexpected = false; }
        quota{&pressure, small.get()};
    {
        graph::debug::PerfTrace::ScopedObserver observer(&quota,
            [](void *owner, std::string_view key, int64_t, bool) noexcept {
                auto &state = *static_cast<QuotaState *>(owner);
                if (key != "native_graph.hybrid.private_map_images" || state.injected) return;
                try { state.pressure->set(state.memory->available()); state.injected = true; }
                catch (...) { state.unexpected = true; }
            });
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), graph::query::execution::MaterializedByteLimitExceeded);
    }
    pressure.set(0); EXPECT_FALSE(quota.unexpected);
    if (conversion.conversion->compact_column_memberships) EXPECT_TRUE(quota.injected);
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    write.memory = memory;
    for (const auto boundary : {std::string_view("native_graph.hybrid.private_map_images"), std::string_view("native_graph.hybrid.pool_frames_prepared")}) {
        CancelAfterPrivatePreparation cancellation; write.cancellation = &cancellation;
        struct State { CancelAfterPrivatePreparation *cancellation; std::string_view boundary; } state{&cancellation, boundary};
        const auto beforeAttempt = io_->dataMutationEpoch();
        {
            graph::debug::PerfTrace::ScopedObserver observe(&state,
                [](void *owner, std::string_view key, int64_t, bool) noexcept {
                    auto &state = *static_cast<State *>(owner); if (key == state.boundary) state.cancellation->prepared = true;
                });
            EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), PrivatePreparationCancelled);
        }
        EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
        if (boundary == "native_graph.hybrid.private_map_images") EXPECT_EQ(io_->dataMutationEpoch(), beforeAttempt);
        else EXPECT_GT(io_->dataMutationEpoch(), beforeAttempt);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    }
    write.cancellation = nullptr;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries.size(), 1U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}

TEST_F(HybridPropertyOrdinaryConversionTest, DurableDemotionPreservesNullSignedZeroAndTheExactNanPayload) {
    seed(NODE); NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto update = planner.beginTransaction(); constexpr uint64_t nanBits = UINT64_C(0x7ff80000abc12345);
    for (uint64_t id = 1; id <= 48; ++id) {
        PropertyValue value(id == 1 ? -0.0 : id == 2 ? std::bit_cast<double>(nanBits) : double(id) + 0.25);
        if (id == 3) value = PropertyValue();
        update.replaceNodeProperty({id, graph::Node::typeId, {{1, value}}});
    }
    ASSERT_TRUE(update.commit().committed()); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 4}, 1, std::monostate{}, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 47U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.extensions, 47U);
    for (const auto *source : {&pin, &old}) {
        const auto zero = properties_->readEntity(*source, {NODE, 1});
        const auto nan = properties_->readEntity(*source, {NODE, 2});
        const auto null = properties_->readEntity(*source, {NODE, 3});
        EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(zero.entries[0].value.getVariant())), UINT64_C(1) << 63);
        EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(nan.entries[0].value.getVariant())), nanBits);
        EXPECT_TRUE(std::holds_alternative<std::monostate>(null.entries[0].value.getVariant()));
    }
    EXPECT_FALSE(properties_->readEntityOptional(pin, {NODE, 4}));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 4}).entries.size(), 1U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(HybridPropertyOrdinaryConversionTest, EdgeFieldDemotionPreservesTheMainEdgeAndGlobalAdjacencyAuthority) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto edge = entities_->readEdge(old, 1);
    const auto adjacency = metadata_->findIndexRootOptional(old, RSS_GLOBAL_ADJACENCY_INDEX_ID);
    const auto original = properties_->readEntity(old, {EDGE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{EDGE, 1}, 1, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot();
    EXPECT_EQ(entities_->readEdge(pin, 1), edge);
    EXPECT_EQ(metadata_->findIndexRootOptional(pin, RSS_GLOBAL_ADJACENCY_INDEX_ID), adjacency);
    EXPECT_EQ(properties_->readEntity(pin, {EDGE, 1}).entries[0].value, PropertyValue(true));
    EXPECT_EQ(properties_->readEntity(old, {EDGE, 1}), original);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 96U); EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 48U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, ExistingInlineExtensionsUpdateAndDeleteWithoutChangingOwnerLiveness) {
    seed(NODE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 3> cells{{
        {{NODE, 1}, 1, true}, {{NODE, 1}, 2, std::monostate{}, true}, {{NODE, 2}, 1, std::monostate{}}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot();
    const auto first = properties_->readEntity(pin, {NODE, 1}); ASSERT_EQ(first.entries.size(), 1U);
    EXPECT_EQ(first.entries[0].value, PropertyValue(true));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(properties_->readEntity(pin, {NODE, 2}).entries[0].value.getVariant()));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 3U); EXPECT_EQ(report.extensions, 3U); EXPECT_EQ(report.columns, 0U);
    EXPECT_EQ(std::popcount(context.groupRecord({NODE, 1})->live), 2);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), first);
}
TEST_F(HybridPropertyOrdinaryConversionTest, InlineAndColumnEditsShareOneAtomicFinalGroupAfterDemotion) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    auto before = metadata_->openSnapshot().superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> demote{{{{NODE, 1}, 1, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        demote, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto old = metadata_->openSnapshot(); before = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 3> cells{{
        {{NODE, 1}, 1, int64_t(999)}, {{NODE, 2}, 1, std::monostate{}, true}, {{NODE, 3}, 2, true}}};
    write.expected_generation = before.generation; write.expected_descriptor = before.hybrid_property_descriptor_root; write.replacements = cells;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 95U); EXPECT_EQ(report.extensions, 95U); EXPECT_EQ(report.columns, 0U);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue(int64_t(999)));
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 2}).entries.size(), 1U);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 3}).entries[1].value, PropertyValue(true));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}).entries[0].value, PropertyValue(true));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, InlineNoopDuplicateAndDeadOwnerRequestsNeverWriteData) {
    seed(NODE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 2> duplicate{{{{NODE, 1}, 1, int64_t(1)}, {{NODE, 1}, 1, int64_t(1)}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        std::span(duplicate).first(1), memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    EXPECT_EQ(metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), before);
    write.replacements = duplicate;
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    const std::array<NativeHybridPropertyColumnReplacement, 1> missing{{{{NODE, 3}, 3, int64_t(1)}}}; write.replacements = missing;
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, RemovingEveryInlineFieldLeavesLiveOwnersAndErasesBothFieldMaps) {
    seed(EDGE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto edge = entities_->readEdge(old, 1);
    const std::array<NativeHybridPropertyColumnReplacement, 4> cells{{
        {{EDGE, 1}, 1, std::monostate{}, true}, {{EDGE, 1}, 2, std::monostate{}, true},
        {{EDGE, 2}, 1, std::monostate{}, true}, {{EDGE, 2}, 2, std::monostate{}, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); EXPECT_FALSE(properties_->readEntityOptional(pin, {EDGE, 1}));
    EXPECT_FALSE(properties_->readEntityOptional(pin, {EDGE, 2})); EXPECT_EQ(entities_->readEdge(pin, 1), edge);
    EXPECT_EQ(properties_->readEntity(old, {EDGE, 1}).entries.size(), 2U);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 0U); EXPECT_EQ(report.extensions, 0U); EXPECT_EQ(std::popcount(context.groupRecord({EDGE, 1})->live), 2);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, InlineMutationABRecoveryKeepsCompleteFieldAndLocatorGenerations) {
    seed(NODE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, true}, {{NODE, 1}, 2, std::monostate{}, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1), publication), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        const auto value = roots.readEntity(pin, {NODE, 1}); EXPECT_EQ(value.entries.size(), installed ? 1U : 2U);
        EXPECT_EQ(value.entries[0].value, installed ? PropertyValue(true) : original.entries[0].value);
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyOrdinaryConversionTest, InlineGrowthRebindsLocatorsAndPreservesOldNullFields) {
    seed(NODE, 2, 26); NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto tx = planner.beginTransaction();
    for (uint64_t id = 1; id <= 2; ++id) {
        PropertyCollectionRoot root{id, graph::Node::typeId, {}};
        for (uint32_t token = 1; token <= 26; ++token) root.entries.push_back({token, PropertyValue()});
        tx.replaceNodeProperty(std::move(root));
    }
    ASSERT_TRUE(tx.commit().committed()); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    std::array<NativeHybridPropertyColumnReplacement, 52> cells; size_t position = 0;
    for (uint64_t id = 1; id <= 2; ++id) for (uint32_t token = 1; token <= 26; ++token)
        cells[position++] = {{NODE, id}, token, int64_t(id * 100 + token)};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot();
    for (uint64_t id = 1; id <= 2; ++id) {
        const auto current = properties_->readEntity(pin, {NODE, id}); const auto previous = properties_->readEntity(old, {NODE, id});
        ASSERT_EQ(current.entries.size(), 26U); ASSERT_EQ(previous.entries.size(), 26U);
        for (size_t token = 0; token < 26; ++token) {
            EXPECT_EQ(current.entries[token].value, PropertyValue(int64_t(id * 100 + token + 1)));
            EXPECT_TRUE(std::holds_alternative<std::monostate>(previous.entries[token].value.getVariant()));
        }
    }
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 52U); EXPECT_EQ(report.extensions, 52U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, InlineMutationCancellationRefundsAndNativeTransactionRetries) {
    seed(NODE, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const auto original = properties_->readEntity(old, {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, true}, {{NODE, 1}, 2, std::monostate{}, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    for (const auto boundary : {std::string_view("native_graph.hybrid.private_map_images"), std::string_view("native_graph.hybrid.pool_frames_prepared")}) {
        CancelAfterPrivatePreparation cancellation; write.cancellation = &cancellation;
        struct State { CancelAfterPrivatePreparation *cancellation; std::string_view boundary; } state{&cancellation, boundary};
        const auto epoch = io_->dataMutationEpoch();
        {
            graph::debug::PerfTrace::ScopedObserver observe(&state, [](void *owner, std::string_view key, int64_t, bool) noexcept {
                auto &state = *static_cast<State *>(owner); if (key == state.boundary) state.cancellation->prepared = true;
            });
            EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), PrivatePreparationCancelled);
        }
        EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
        if (boundary == "native_graph.hybrid.private_map_images") EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        else EXPECT_GT(io_->dataMutationEpoch(), epoch);
    }
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto tx = planner.beginTransaction(); tx.stageHybridPropertyColumns(cells, memory, nullptr, true);
    ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    const auto value = properties_->readEntity(metadata_->openSnapshot(), {NODE, 1});
    ASSERT_EQ(value.entries.size(), 1U); EXPECT_EQ(value.entries[0].value, PropertyValue(true));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
TEST_F(HybridPropertyOrdinaryConversionTest, MissingColumnInsertionAndRemovalMergeBeforeTheFinalThreshold) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 4> cells{{
        {{NODE, 1}, 1, std::monostate{}, true}, {{NODE, 49}, 1, int64_t(123)},
        {{NODE, 49}, 2, std::monostate{}}, {{NODE, 49}, 3, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 98U); EXPECT_EQ(report.columns, 2U); EXPECT_EQ(report.extensions, 1U);
    const auto root = properties_->readEntity(pin, {NODE, 49}); ASSERT_EQ(root.entries.size(), 3U);
    EXPECT_EQ(root.entries[0].value, PropertyValue(int64_t(123))); EXPECT_EQ(root.entries[1].value, PropertyValue());
    EXPECT_EQ(root.entries[2].value, PropertyValue(true)); EXPECT_FALSE(properties_->readEntityOptional(old, {NODE, 49}));
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries.size(), 1U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 49}), root);
}
TEST_F(HybridPropertyOrdinaryConversionTest, MissingIncompatibleColumnMemberDemotesAllFinalMembers) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto edge = entities_->readEdge(old, 49);
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{EDGE, 49}, 1, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 97U); EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 49U);
    EXPECT_EQ(properties_->readEntity(pin, {EDGE, 49}).entries[0].value, PropertyValue(true));
    EXPECT_FALSE(properties_->readEntityOptional(old, {EDGE, 49})); EXPECT_EQ(entities_->readEdge(pin, 49), edge);
    EXPECT_EQ(properties_->readEntity(pin, {EDGE, 1}), properties_->readEntity(old, {EDGE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, EmptyLiveGroupCreatesItsFirstFieldMapsAndMissingEraseIsNoop) {
    seed(NODE, 1, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock(); const auto epoch = io_->dataMutationEpoch();
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 5, std::monostate{}, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    EXPECT_EQ(metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    cells[0].erase = false;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); const auto root = properties_->readEntity(pin, {NODE, 1});
    ASSERT_EQ(root.entries.size(), 1U); EXPECT_EQ(root.entries[0].key_token, 5U); EXPECT_EQ(root.entries[0].value, PropertyValue());
    EXPECT_FALSE(properties_->readEntityOptional(old, {NODE, 1}));
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 1U); EXPECT_EQ(report.extensions, 1U); EXPECT_EQ(report.columns, 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, DenseFieldInsertionABRecoveryKeepsExtensionsAndExactDoubleBits) {
    seed(NODE, 64, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    constexpr uint64_t nanBits = UINT64_C(0x7ff80000abc12345);
    std::array<NativeHybridPropertyColumnReplacement, 64> cells;
    for (size_t i = 0; i < cells.size(); ++i) cells[i] = {{NODE, i + 1}, 1, double(i)};
    cells[0].value = -0.0; cells[1].value = std::bit_cast<double>(nanBits); cells[2].value = std::monostate{};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1), publication), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        HybridPropertyReadContext context(io_, pages, *recovered, pin);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
        EXPECT_EQ(report.memberships, installed ? 64U : 0U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.extensions, installed ? 64U : 0U);
        if (installed) {
            const auto zero = roots.readEntity(pin, {NODE, 1}); const auto nan = roots.readEntity(pin, {NODE, 2});
            EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(zero.entries[0].value.getVariant())), UINT64_C(1) << 63);
            EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(nan.entries[0].value.getVariant())), nanBits);
            EXPECT_EQ(roots.readEntity(pin, {NODE, 3}).entries[0].value, PropertyValue());
        }
        EXPECT_FALSE(properties_->readEntityOptional(old, {NODE, 1}));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyOrdinaryConversionTest, SparseFieldDoesNotRepromoteAt48WithoutExplicitConversion) {
    seed(NODE, 48, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    auto before = metadata_->openSnapshot().superblock();
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, std::monostate{}, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto sparse = metadata_->openSnapshot(); before = sparse.superblock(); cells[0].erase = false; cells[0].value = int64_t(77);
    write.expected_generation = before.generation; write.expected_descriptor = before.hybrid_property_descriptor_root;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 48U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.extensions, 48U);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue(int64_t(77)));
    EXPECT_FALSE(properties_->readEntityOptional(sparse, {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    conversion.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(conversion)); EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(77)));
    conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion)); const auto explicitPin = metadata_->openSnapshot();
    HybridPropertyReadContext explicitContext(io_, *slotted_pages_, *metadata_, explicitPin);
    const auto explicitReport = HybridPropertyDirectoryIntegrity::verify(explicitContext, explicitPin, memory);
    EXPECT_EQ(explicitReport.columns, 1U); EXPECT_EQ(explicitReport.extensions, 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, HomogeneousExtensionsAndNewDenseFieldsDoNotAutomaticallyPromote) {
    seed(EDGE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(EDGE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    auto before = metadata_->openSnapshot().superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> demote{{{{EDGE, 1}, 1, true}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        demote, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto old = metadata_->openSnapshot(); before = old.superblock(); const auto edge = entities_->readEdge(old, 1);
    std::array<NativeHybridPropertyColumnReplacement, 50> cells;
    cells[0] = {{EDGE, 1}, 1, int64_t(1)};
    for (size_t id = 1; id <= 49; ++id) cells[id] = {{EDGE, id}, 3, true};
    write.expected_generation = before.generation; write.expected_descriptor = before.hybrid_property_descriptor_root; write.replacements = cells;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 145U); EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 97U);
    EXPECT_EQ(properties_->readEntity(pin, {EDGE, 1}).entries[0].value, PropertyValue(int64_t(1)));
    EXPECT_EQ(properties_->readEntity(old, {EDGE, 1}).entries[0].value, PropertyValue(true));
    EXPECT_EQ(entities_->readEdge(pin, 1), edge);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, AllNullExtensionsRemainExtensionsAfterAScalarTypeAppears) {
    seed(NODE, 48, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    auto before = metadata_->openSnapshot().superblock();
    std::array<NativeHybridPropertyColumnReplacement, 48> cells;
    for (size_t i = 0; i < cells.size(); ++i) cells[i] = {{NODE, i + 1}, 1, std::monostate{}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto old = metadata_->openSnapshot();
    { HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, old);
      const auto report = HybridPropertyDirectoryIntegrity::verify(context, old, memory);
      EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.extensions, 48U); }
    before = old.superblock(); write.expected_generation = before.generation; write.expected_descriptor = before.hybrid_property_descriptor_root;
    cells[0].value = int64_t(42); write.replacements = std::span(cells).first(1);
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 48U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.extensions, 48U);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue(int64_t(42)));
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 2}).entries[0].value, PropertyValue());
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}).entries[0].value, PropertyValue());
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, ExtensionBatchQuotaAndCancellationPreserveTheSourceWithoutPromotion) {
    seed(NODE, 48, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
    const auto initial = metadata_->openSnapshot().superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> demotion{{{{NODE, 1}, 1, true}}};
    NativeHybridPropertyColumnWrite demote{initial.database_id, initial.generation, initial.hybrid_property_descriptor_root,
        demotion, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(demote, LogSequenceNumber(initial.checkpoint_lsn.value() + 1)));
    const auto before = metadata_->openSnapshot().superblock();
    std::array<NativeHybridPropertyColumnReplacement, 48> cells;
    for (size_t i = 0; i < cells.size(); ++i) cells[i] = {{NODE, i + 1}, 1, int64_t(i)};
    auto small = std::make_shared<MaterializedByteBudget>(128 * 1024);
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, small, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch); write.memory = memory;
    for (const auto boundary : {std::string_view("native_graph.hybrid.private_map_images"), std::string_view("native_graph.hybrid.pool_frames_prepared")}) {
        CancelAfterPrivatePreparation cancellation; write.cancellation = &cancellation;
        struct State { CancelAfterPrivatePreparation *cancellation; std::string_view boundary; } state{&cancellation, boundary};
        const auto epoch = io_->dataMutationEpoch();
        {
            graph::debug::PerfTrace::ScopedObserver observe(&state, [](void *owner, std::string_view key, int64_t, bool) noexcept {
                auto &state = *static_cast<State *>(owner); if (key == state.boundary) state.cancellation->prepared = true;
            });
            EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), PrivatePreparationCancelled);
        }
        EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
        if (boundary == "native_graph.hybrid.private_map_images") EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        else EXPECT_GT(io_->dataMutationEpoch(), epoch);
    }
    write.cancellation = nullptr;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.memberships, 48U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.extensions, 48U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyOrdinaryConversionTest, ExtensionNoopUsesRowEqualityForSignedZeroAndNan) {
    seed(NODE, 2, 0);
    constexpr uint64_t nanBits = UINT64_C(0x7ff80000abc12345);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto tx = planner.beginTransaction();
    tx.replaceNodeProperty({1, graph::Node::typeId, {{1, PropertyValue(0.0)}}});
    tx.replaceNodeProperty({2, graph::Node::typeId, {{1, PropertyValue(std::bit_cast<double>(nanBits))}}});
    ASSERT_TRUE(tx.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    ASSERT_NO_THROW(run(conversion));
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, -0.0}}};
    NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    EXPECT_EQ(metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    auto zero = properties_->readEntity(metadata_->openSnapshot(), {NODE, 1});
    EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(zero.entries[0].value.getVariant())), UINT64_C(0));
    cells[0] = {{NODE, 2}, 1, std::bit_cast<double>(nanBits)};
    const auto after = metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
    EXPECT_GT(after.generation.value(), before.generation.value());
    EXPECT_GT(io_->dataMutationEpoch(), epoch);
    const auto nan = properties_->readEntity(metadata_->openSnapshot(), {NODE, 2});
    EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(nan.entries[0].value.getVariant())), nanBits);
    EXPECT_EQ(memory->used(), 0U);
}
#endif
