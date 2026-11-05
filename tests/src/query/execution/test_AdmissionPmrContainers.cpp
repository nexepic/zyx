#include <gtest/gtest.h>
#include "graph/utils/AdmissionPmrContainers.hpp"
#include "graph/utils/StandardContainerHeapBytes.hpp"
#include "query/execution/BudgetMemoryResource.hpp"

namespace graph::query::execution::detail {
namespace {
class AdmissionRequests final : public std::pmr::memory_resource {
public:
    size_t calls=0U, live=0U, fail_at=SIZE_MAX;
    bool deny=false;
private:
    void *do_allocate(size_t bytes,size_t alignment) override {
        if(calls++==fail_at || deny) throw std::bad_alloc();
        auto *pointer=std::pmr::new_delete_resource()->allocate(bytes,alignment); live+=bytes; return pointer;
    }
    void do_deallocate(void *pointer,size_t bytes,size_t alignment) override {
        live-=bytes; std::pmr::new_delete_resource()->deallocate(pointer,bytes,alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this==&other; }
};
}

TEST(AdmissionPmrContainersTest, AllEmptyMetadataContainersAcceptADenyingResourceWithoutHiddenRequests) {
    AdmissionRequests memory; memory.deny=true;
    {
        utils::AdmissionPmrVector<uint64_t> vector(&memory);
        utils::AdmissionPmrMap<uint64_t,uint64_t> ordered(&memory);
        utils::AdmissionPmrSet<uint64_t> orderedSet(&memory);
        utils::AdmissionPmrUnorderedMap<uint64_t,uint64_t> hashed(&memory);
        utils::AdmissionPmrUnorderedSet<uint64_t> hashedSet(&memory);
        EXPECT_TRUE(vector.empty()); EXPECT_TRUE(ordered.empty()); EXPECT_TRUE(orderedSet.empty());
        EXPECT_THROW(static_cast<void>(ordered.at(1U)),std::out_of_range);
        EXPECT_THROW(static_cast<void>(std::as_const(ordered).at(1U)),std::out_of_range);
        EXPECT_TRUE(hashed.empty()); EXPECT_TRUE(hashedSet.empty()); EXPECT_EQ(memory.calls,0U);
    }
    EXPECT_EQ(memory.calls,0U); EXPECT_EQ(memory.live,0U);
}

TEST(AdmissionPmrContainersTest, VectorRejectionPreservesValuesAndReleaseNeedsNoRemainingCredit) {
    MaterializedByteBudget budget(64U); AdmissionRequests upstream; BudgetMemoryResource memory(budget,&upstream);
    utils::AdmissionPmrVector<uint64_t> values(&memory); values.reserve(8U); values.assign(8U,42U);
    const auto calls=upstream.calls;
    EXPECT_THROW(values.push_back(9U),MaterializedByteLimitExceeded);
    EXPECT_EQ(values.size(),8U); EXPECT_EQ(values.front(),42U); EXPECT_EQ(budget.used(),64U);
    EXPECT_EQ(upstream.calls,calls);
    upstream.deny=true;
    utils::AdmissionPmrVector<uint64_t>(&memory).swap(values);
    EXPECT_EQ(budget.used(),0U); EXPECT_EQ(upstream.live,0U); EXPECT_EQ(upstream.calls,calls);
}

TEST(AdmissionPmrContainersTest, NestedOwningKeysAndMatchVectorsKeepTheSameResourceAndRefundAllStorage) {
    MaterializedByteBudget budget(8192U); AdmissionRequests upstream; BudgetMemoryResource memory(budget,&upstream);
    {
        utils::AdmissionPmrUnorderedMap<std::pmr::string,utils::AdmissionPmrVector<uint64_t>> matches(&memory);
        std::pmr::string key(std::string_view("owner"),&memory);
        auto &values=matches[key]; values.push_back(7U); values.push_back(8U);
        EXPECT_EQ(values.get_allocator().resource(),&memory);
        EXPECT_EQ(matches.begin()->first.get_allocator().resource(),&memory);
        EXPECT_EQ(budget.used(),upstream.live);
        const auto retained=budget.used(),calls=upstream.calls;
        EXPECT_THROW(values.reserve(8192U),MaterializedByteLimitExceeded);
        EXPECT_EQ(values.size(),2U); EXPECT_EQ(values.at(1U),8U);
        EXPECT_EQ(budget.used(),retained); EXPECT_EQ(upstream.calls,calls);
    }
    EXPECT_EQ(budget.used(),0U); EXPECT_EQ(upstream.live,0U);
}

TEST(AdmissionPmrContainersTest, EveryUpstreamFailurePropagatesAndRefundsPreviouslyConstructedMetadata) {
    const auto populate=[](std::pmr::memory_resource *memory) {
        utils::AdmissionPmrVector<uint64_t> vector(memory); vector.reserve(8U); vector.push_back(1U);
        utils::AdmissionPmrMap<uint64_t,uint64_t> ordered(memory); ordered.emplace(1U,2U);
        utils::AdmissionPmrSet<uint64_t> orderedSet(memory); orderedSet.emplace(3U);
        utils::AdmissionPmrUnorderedMap<uint64_t,uint64_t> hashed(memory); hashed.reserve(8U); hashed.emplace(4U,5U);
        utils::AdmissionPmrUnorderedSet<uint64_t> hashedSet(memory); hashedSet.reserve(8U); hashedSet.emplace(6U);
    };
    MaterializedByteBudget budget(65536U); AdmissionRequests baseline;
    { BudgetMemoryResource memory(budget,&baseline); populate(&memory); }
    ASSERT_GT(baseline.calls,0U); EXPECT_EQ(budget.used(),0U);
    for(size_t failure=0;failure<baseline.calls;++failure) {
        SCOPED_TRACE(failure); AdmissionRequests upstream; upstream.fail_at=failure;
        BudgetMemoryResource memory(budget,&upstream);
        EXPECT_THROW(populate(&memory),std::bad_alloc);
        EXPECT_EQ(budget.used(),0U); EXPECT_EQ(upstream.live,0U);
    }
}

TEST(AdmissionPmrContainersTest, EmptyStringProxyRejectionEscapesAndDoesNotLeakItsAdmission) {
    MaterializedByteBudget budget(1U); AdmissionRequests upstream; BudgetMemoryResource memory(budget,&upstream);
    bool rejected=false;
    try { const auto empty=utils::makeEmptyAdmissionPmrString(&memory); EXPECT_TRUE(empty.empty()); }
    catch(const MaterializedByteLimitExceeded &) { rejected=true; }
    EXPECT_EQ(rejected,utils::detail::standardContainerProxyBytes()!=0U);
    EXPECT_EQ(budget.used(),0U); EXPECT_EQ(upstream.calls,0U); EXPECT_EQ(upstream.live,0U);
    EXPECT_THROW(static_cast<void>(utils::makeEmptyAdmissionPmrString(nullptr)),std::invalid_argument);
}
TEST(AdmissionPmrContainersTest, StandardPmrInteropRetainsItsRealProxyAndRefundsRejectedGrowth) {
    MaterializedByteBudget budget(1024U); AdmissionRequests upstream; BudgetMemoryResource memory(budget,&upstream);
    {
        // Standard PMR is still interoperable where its fallible constructor
        // has sufficient credit; all real iterator metadata is charged too.
        std::pmr::vector<uint64_t> values(&memory);
        values.reserve(8U); values.assign(8U,42U);
        const auto retained=budget.used(), calls=upstream.calls;
        EXPECT_EQ(retained,upstream.live); EXPECT_GE(retained,64U);
        EXPECT_THROW(values.reserve(2048U),MaterializedByteLimitExceeded);
        EXPECT_EQ(values.size(),8U); EXPECT_EQ(values.back(),42U);
        EXPECT_EQ(budget.used(),retained); EXPECT_EQ(upstream.calls,calls);
    }
    EXPECT_EQ(budget.used(),0U); EXPECT_EQ(upstream.live,0U);
}

} // namespace graph::query::execution::detail
