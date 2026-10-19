#include <gtest/gtest.h>
#include "../../../../../benchmarks/architecture-design/property-subblock/ReservedFieldTransition.hpp"
using namespace zyx::benchmark::subblock;
using namespace graph::query::execution;
using namespace graph::query::execution::detail;
namespace {
std::shared_ptr<ResourceAccount> transitionAccount(size_t bytes) {
 auto process=std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(bytes));
 auto database=std::make_shared<DatabaseResourceBudget>(process,ResourceLimits::uniform(bytes));
 return std::make_shared<ResourceAccount>(database,ResourceClass::RCK_QUERY,bytes);
}
Column transitionSource() {
 Column source;source.live=source.presence=UINT64_MAX;source.words.fill(1);return source;
}
TEST(ReservedPropertySubblockTransitionTest, HoldsSharedAccountUntilPlanDiesAndRefundsOnKernelFailure) {
 auto account=transitionAccount(32768);MaterializedByteBudget budget(32768,account);
 const auto source=transitionSource();
 {ReservedFieldTransition plan(budget,source,0,{CellKind::HFC_INT64,2},UINT64_MAX,2,3968);
 EXPECT_EQ(plan.get().action,FieldAction::HFT_REPLACE_COLUMN);
 RecordProperty("held_transition_reservation_bytes",std::to_string(plan.retainedCharge()));
 EXPECT_EQ(budget.used(),plan.retainedCharge());EXPECT_EQ(account->used(),plan.retainedCharge());
 EXPECT_EQ(source.words[0],1U);}
 EXPECT_EQ(budget.used(),0U);EXPECT_EQ(account->used(),0U);
 EXPECT_THROW((ReservedFieldTransition(budget,source,0,{CellKind::HFC_INT64,2},UINT64_MAX,2,575)),std::length_error);
 EXPECT_EQ(budget.used(),0U);EXPECT_EQ(account->used(),0U);
 EXPECT_THROW((ReservedFieldTransition(budget,source,64,{},UINT64_MAX,2,3968)),std::invalid_argument);
 EXPECT_EQ(account->used(),0U);
}
TEST(ReservedPropertySubblockTransitionTest, LocalAndParentAdmissionFailBeforeChangeAndAllowRetry) {
 const auto source=transitionSource();auto account=transitionAccount(32768);
 MaterializedByteBudget local(1,account);
 EXPECT_THROW((ReservedFieldTransition(local,source,0,{CellKind::HFC_INT64,2},UINT64_MAX,2,3968)),MaterializedByteLimitExceeded);
 EXPECT_EQ(account->used(),0U);
 auto small=transitionAccount(1);MaterializedByteBudget large(32768,small);
 EXPECT_THROW((ReservedFieldTransition(large,source,0,{CellKind::HFC_INT64,2},UINT64_MAX,2,3968)),ResourceAdmissionDenied);
 EXPECT_EQ(small->used(),0U);EXPECT_EQ(large.used(),0U);
 MaterializedByteBudget allowed(32768,account);
 {ReservedFieldTransition plan(allowed,source,0,{CellKind::HFC_DOUBLE,0},UINT64_MAX,2,3968);
 EXPECT_EQ(plan.get().action,FieldAction::HFT_DEMOTE_FIELD);EXPECT_GT(account->used(),0U);}
 EXPECT_EQ(account->used(),0U);EXPECT_EQ(source.words[0],1U);
}
}
