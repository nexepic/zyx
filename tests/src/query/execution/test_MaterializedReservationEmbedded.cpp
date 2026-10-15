#include <gtest/gtest.h>
#include "graph/query/execution/MaterializedByteBudget.hpp"
using namespace graph::query::execution;
TEST(MaterializedReservationEmbeddedTest, TransferRetainsLogicalSizeWithoutDoubleChargingItsAllocatedParent) {
	MaterializedByteBudget budget(512U);void *frame=budget.allocate(64U,alignof(std::max_align_t));
	MaterializedByteReservation source(&budget);source.setAccounting(100U,64U);
	EXPECT_EQ(source.bytes(),100U);EXPECT_EQ(budget.used(),100U);
	auto target=source.splitOffAccounting(100U,64U);
	EXPECT_EQ(source.bytes(),0U);EXPECT_EQ(target.bytes(),100U);EXPECT_EQ(budget.used(),100U);
	// Reclassify before an untracked destination takes ownership of the parent.
	target.setAccounting(100U,0U);EXPECT_EQ(budget.used(),164U);
	budget.deallocate(frame,64U,alignof(std::max_align_t));EXPECT_EQ(budget.used(),100U);
	target.reset();EXPECT_EQ(budget.used(),0U);
}
TEST(MaterializedReservationEmbeddedTest, FailedReclassificationAndInvalidTransfersPreserveBothOwners) {
	MaterializedByteBudget budget(100U);void *frame=budget.allocate(64U,alignof(std::max_align_t));
	MaterializedByteReservation source(&budget);source.setAccounting(100U,64U);
	EXPECT_THROW(source.setAccounting(100U,0U),MaterializedByteLimitExceeded);
	EXPECT_EQ(source.bytes(),100U);EXPECT_EQ(budget.used(),100U);
	EXPECT_THROW((void)source.splitOffAccounting(80U,0U),std::logic_error);
	EXPECT_EQ(source.bytes(),100U);EXPECT_EQ(budget.used(),100U);
	source.reset();budget.deallocate(frame,64U,alignof(std::max_align_t));EXPECT_EQ(budget.used(),0U);
}
