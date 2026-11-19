#include <gtest/gtest.h>
#include "query/execution/ProcedurePayloadAdmission.hpp"

namespace graph::query::execution::detail {
TEST(ProcedurePayloadAdmissionTest, PreparedHashBucketsAreChargedOnceAndNeverShrunkByInsertion) {
    using Map = std::unordered_map<uint64_t, uint64_t>;
    auto budget = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    {
        ProcedurePayloadCredit credit(budget.get(), procedureEmptyMapHeapUpper<Map>());
        ProcedurePayloadAdmissionScope scope(credit);
        Map values;
        procedureMapReserve(values, 32U);
        const auto buckets = values.bucket_count();
        const auto prepared = credit.bytes();
        ASSERT_GE(buckets, 32U);
        procedureMapReserve(values, 16U);
        EXPECT_EQ(values.bucket_count(), buckets);
        EXPECT_EQ(credit.bytes(), prepared);
        ASSERT_TRUE(procedureMapEmplace(values, uint64_t{7}, uint64_t{9}).second);
        EXPECT_EQ(values.bucket_count(), buckets);
        const size_t node = utils::detail::standardAllocatorRequestBytes(
            utils::detail::standardHashNodeBytes<Map::value_type>(), alignof(Map::value_type));
        EXPECT_EQ(credit.bytes() - prepared, node);
        EXPECT_EQ(values.at(7U), 9U);
        // Once the row is populated, a smaller request still cannot duplicate
        // the bucket grant or invalidate the existing table's storage.
        procedureMapReserve(values, 1U);
        EXPECT_EQ(values.bucket_count(), buckets);
        EXPECT_EQ(credit.bytes(), prepared + node);
    }
    EXPECT_EQ(budget->used(), 0U);
}

TEST(ProcedurePayloadAdmissionTest, MissingHashCapacityStillRequiresAdmissionBeforeRehash) {
    using Map = std::unordered_map<uint64_t, uint64_t>;
    auto budget = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    {
        ProcedurePayloadCredit credit(budget.get(), procedureEmptyMapHeapUpper<Map>());
        ProcedurePayloadAdmissionScope scope(credit);
        Map values;
        procedureMapReserve(values, 4U);
        const auto before = credit.bytes();
        const auto oldBuckets = values.bucket_count();
        procedureMapReserve(values, oldBuckets + 1U);
        EXPECT_GT(values.bucket_count(), oldBuckets);
        EXPECT_GT(credit.bytes(), before); // Old/new bucket overlap stays charged.
    }
    EXPECT_EQ(budget->used(), 0U);
}

TEST(ProcedurePayloadAdmissionTest, ReusingBucketsDoesNotBypassNodeAdmissionAndAllowsRetryAfterRefund) {
    using Map = std::unordered_map<uint64_t, uint64_t>;
    auto budget = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    {
        ProcedurePayloadCredit credit(budget.get(), procedureEmptyMapHeapUpper<Map>());
        ProcedurePayloadAdmissionScope scope(credit);
        Map values;
        procedureMapReserve(values, 8U);
        const auto before = credit.bytes(), buckets = values.bucket_count();
        MaterializedByteReservation sibling(budget.get());
        sibling.set(budget->available());
        EXPECT_THROW(procedureMapEmplace(values, uint64_t{7}, uint64_t{9}), MaterializedByteLimitExceeded);
        EXPECT_TRUE(values.empty());
        EXPECT_EQ(values.bucket_count(), buckets);
        EXPECT_EQ(credit.bytes(), before);
        sibling.reset();
        EXPECT_TRUE(procedureMapEmplace(values, uint64_t{7}, uint64_t{9}).second);
        EXPECT_EQ(values.at(7U), 9U);
    }
    EXPECT_EQ(budget->used(), 0U);
}
}
