#include <gtest/gtest.h>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/index/FixedBTreeMapMutation.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
namespace {
using Map = FixedBTreeMap<8, 8>;
using Mutation = FixedBTreeMapMutation<8, 8>;
constexpr uint64_t MAP_ID = 0x48504D555441ULL;
Map::Key key(uint8_t id) { Map::Key result{}; result.back() = std::byte{id}; return result; }
Map::Value value(uint8_t id) { Map::Value result{}; result.front() = std::byte{id}; return result; }
NativeGraphWritePlannerOptions options() {
    NativeGraphWritePlannerOptions result;
    result.first_slot = PhysicalSlotId(40); result.last_slot = PhysicalSlotId(80);
    result.entity_pool_owner_id = 901;
    result.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    return result;
}
class PropertySubblockMapMutationTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    void create() {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
        auto transaction = planner.beginTransaction();
        const auto bytes = Map::encode(key(1), value(1));
        BTreePageEntry entry{{bytes.begin(), bytes.end()}, 0};
        auto plan = btree_pages_->planInitialTreeMutation(transaction.baseSnapshot(), MAP_ID,
            std::span(&entry, 1), {}, BTreeLeafValueLayout::BLVL_EMPTY);
        transaction.stageScalarIndexCreate(*btree_pages_, std::move(plan));
        ASSERT_TRUE(transaction.commit().committed());
    }
};
TEST_F(PropertySubblockMapMutationTest, RepeatedSetRemoveSetPublishesExactlyOneFinalSuffixAndOldPinSurvives) {
    create(); auto old = metadata_->openSnapshot();
    MaterializedByteBudget budget(65536);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    auto transaction = planner.beginTransaction();
    {
        Mutation mutation(transaction, *btree_pages_, *metadata_, MAP_ID, 4, budget);
        mutation.set(key(1), value(2)); mutation.erase(key(1)); mutation.set(key(1), value(3));
        mutation.set(key(2), value(4)); mutation.set(key(2), value(5));
        EXPECT_EQ(mutation.changedPrefixCount(), 2U); EXPECT_GT(budget.used(), 0U);
        mutation.stage();
        EXPECT_THROW(mutation.set(key(1), value(9)), std::logic_error);
        EXPECT_THROW(mutation.stage(), std::logic_error);
        ASSERT_TRUE(transaction.commit().committed());
    }
    EXPECT_EQ(budget.used(), 0U);
    Map map(MAP_ID, *btree_pages_, *metadata_); auto fresh = metadata_->openSnapshot();
    EXPECT_EQ(map.find(old, key(1)), value(1)); EXPECT_FALSE(map.find(old, key(2)));
    EXPECT_EQ(map.find(fresh, key(1)), value(3)); EXPECT_EQ(map.find(fresh, key(2)), value(5));
    fresh.release(); old.release(); reopenNativeStores();
    Map reopened(MAP_ID, *btree_pages_, *metadata_); auto recovered = metadata_->openSnapshot();
    EXPECT_EQ(reopened.find(recovered, key(1)), value(3));
    EXPECT_EQ(reopened.find(recovered, key(2)), value(5));
}
TEST_F(PropertySubblockMapMutationTest, FinalNoOpAndInsertThenEraseDoNotStagePhysicalChanges) {
    create(); const auto before = metadata_->openSnapshot().superblock();
    MaterializedByteBudget budget(65536);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    auto transaction = planner.beginTransaction();
    Mutation mutation(transaction, *btree_pages_, *metadata_, MAP_ID, 2, budget);
    mutation.set(key(1), value(2)); mutation.set(key(1), value(1));
    mutation.set(key(2), value(2)); mutation.erase(key(2));
    EXPECT_EQ(mutation.changedPrefixCount(), 0U); mutation.stage();
    // Native transactions deliberately reject empty commits. The coalescer
    // must leave this contract intact rather than invent a metadata write.
    EXPECT_THROW(transaction.commit(), FormatException);
    transaction.rollback();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(PropertySubblockMapMutationTest, RejectedNewPrefixKeepsPreviousFinalStateAndRefundsPlanningNodes) {
    create(); MaterializedByteBudget budget(65536), tooSmall(1);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    auto transaction = planner.beginTransaction();
    {
        Mutation refused(transaction, *btree_pages_, *metadata_, MAP_ID, 1, tooSmall);
        EXPECT_THROW(refused.set(key(1), value(2)), MaterializedByteLimitExceeded);
        EXPECT_EQ(refused.changedPrefixCount(), 0U); EXPECT_EQ(tooSmall.used(), 0U);
        Mutation mutation(transaction, *btree_pages_, *metadata_, MAP_ID, 1, budget);
        mutation.set(key(1), value(2));
        EXPECT_THROW(mutation.set(key(2), value(3)), FormatException);
        mutation.set(key(1), value(4)); mutation.stage();
        ASSERT_TRUE(transaction.commit().committed());
    }
    EXPECT_EQ(budget.used(), 0U);
    Map map(MAP_ID, *btree_pages_, *metadata_); auto snapshot = metadata_->openSnapshot();
    EXPECT_EQ(map.find(snapshot, key(1)), value(4)); EXPECT_FALSE(map.find(snapshot, key(2)));
}
TEST_F(PropertySubblockMapMutationTest, NativeStageFailureRollsBackWholeTransactionRatherThanHalfPublishingMap) {
    create(); MaterializedByteBudget budget(65536);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    auto transaction = planner.beginTransaction();
    // Deliberate conflicting leaf layout exercises failure after staging began.
    const auto bytes = Map::encode(key(1), value(1));
    transaction.stageScalarIndexErase(*btree_pages_, MAP_ID, bytes, BTreeLeafValueLayout::BLVL_U64);
    Mutation mutation(transaction, *btree_pages_, *metadata_, MAP_ID, 2, budget);
    mutation.set(key(1), value(2)); mutation.set(key(2), value(3));
    EXPECT_THROW(mutation.stage(), FormatException);
    EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK);
    EXPECT_THROW(transaction.commit(), FormatException);
    auto snapshot = metadata_->openSnapshot(); Map map(MAP_ID, *btree_pages_, *metadata_);
    EXPECT_EQ(map.find(snapshot, key(1)), value(1)); EXPECT_FALSE(map.find(snapshot, key(2)));
}
} // namespace
