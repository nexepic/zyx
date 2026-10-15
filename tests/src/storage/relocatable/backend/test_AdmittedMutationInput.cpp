#include <gtest/gtest.h>
#include "storage/relocatable/backend/AdmittedMutationInput.hpp"
#include "graph/utils/OwnedVector.hpp"

using namespace graph;
using namespace graph::storage;
using namespace graph::query::execution;
namespace {
class CopyCountingResource final : public std::pmr::memory_resource {
public:
    size_t allocations = 0U;
private:
    void *do_allocate(size_t bytes, size_t alignment) override {
        ++allocations; return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
LogicalGraphMutationBatch inputBatch() {
    PropertyValue::MapType map; map.reserve(1024U);
    map.emplace("text", PropertyValue(std::string(8192U, 'v')));
    map.emplace("bytes", PropertyValue(PropertyValue::Bytes(4096U, std::byte{0x5a})));
    LogicalGraphMutationBatch batch;
    batch.node_creates.push_back({{std::string(1000U, 'l')}, {{"nested", PropertyValue(std::move(map))}}});
    return batch;
}
}

TEST(AdmittedMutationInputTest, FreshCopyOwnsAnIndependentPayloadAndRefundsOnlyAfterDestruction) {
    auto budget = std::make_shared<MaterializedByteBudget>(262144U);
    auto source = inputBatch();
    {
        auto copy = graph::storage::detail::AdmittedMutationInput::copy(budget, source, {});
        EXPECT_EQ(copy->batch.node_creates, source.node_creates);
        EXPECT_GE(budget->used(), copy->copyBytes().total());
        source.node_creates[0].properties[0].value = PropertyValue(9);
        EXPECT_EQ(copy->batch.node_creates[0].properties[0].value.getMap().at("text"), PropertyValue(std::string(8192U, 'v')));
    }
    EXPECT_EQ(budget->used(), 0U);
}

TEST(AdmittedMutationInputTest, OversizedCopyIsRejectedBeforeAllocatingItsPrivateControlObject) {
    CopyCountingResource upstream;
    auto budget = std::make_shared<MaterializedByteBudget>(1024U);
    // Attach a counting parent-independent resource through the coordinated ctor.
    using namespace graph::query::execution::detail;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(65536U));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(65536U));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_WRITE, 65536U);
    budget = std::make_shared<MaterializedByteBudget>(1024U, account, &upstream);
    const auto source = inputBatch();
    EXPECT_THROW(static_cast<void>(graph::storage::detail::AdmittedMutationInput::copy(budget, source, {})), MaterializedByteLimitExceeded);
    EXPECT_EQ(upstream.allocations, 0U); EXPECT_EQ(budget->used(), 0U); EXPECT_EQ(account->used(), 0U);
    EXPECT_EQ(source.node_creates[0].properties[0].value.getMap().size(), 2U);
}

TEST(AdmittedMutationInputTest, NestedCreditTransfersWithAdoptedVectorsAndRejectsForeignReceiversAtomically) {
    auto budget = std::make_shared<MaterializedByteBudget>(262144U);
    {
    MaterializedByteReservation retained(budget.get());
    graph::utils::OwnedVector<LogicalGraphNodeCreateRequest> nodes{graph::utils::OwnedPmrAllocator<LogicalGraphNodeCreateRequest>(budget)};
    auto copy = graph::storage::detail::AdmittedMutationInput::copy(budget, inputBatch(), {});
    const auto before = budget->used();
    MaterializedByteBudget other(262144U); MaterializedByteReservation foreign(&other);
    EXPECT_THROW(copy->transferNestedTo(foreign), std::logic_error);
    EXPECT_EQ(budget->used(), before); EXPECT_EQ(other.used(), 0U);
    nodes.reserve(copy->batch.node_creates.size());
    nodes.insert(nodes.end(), std::make_move_iterator(copy->batch.node_creates.begin()), std::make_move_iterator(copy->batch.node_creates.end()));
    copy->transferNestedTo(retained);
    EXPECT_THROW(copy->transferNestedTo(retained), std::logic_error);
    copy.reset();
    ASSERT_EQ(nodes.size(), 1U); EXPECT_GT(retained.bytes(), 8192U);
    EXPECT_EQ(nodes[0].properties[0].value.getMap().at("bytes"), PropertyValue(PropertyValue::Bytes(4096U, std::byte{0x5a})));
    }
    EXPECT_EQ(budget->used(), 0U);
}

TEST(AdmittedMutationInputTest, InvalidDepthAndCancellationLeaveNoPrivateCharge) {
    auto budget = std::make_shared<MaterializedByteBudget>(262144U);
    LogicalGraphMutationBatch batch;
    PropertyValue nested(7);
    for (size_t depth = 0U; depth < 66U; ++depth) nested = PropertyValue(std::vector<PropertyValue>{std::move(nested)});
    batch.node_creates.push_back({{}, {{"deep", std::move(nested)}}});
    EXPECT_THROW(static_cast<void>(graph::storage::detail::AdmittedMutationInput::copy(budget, batch, {})), std::length_error);
    EXPECT_EQ(budget->used(), 0U);
    const auto source = inputBatch();
    EXPECT_THROW(static_cast<void>(graph::storage::detail::AdmittedMutationInput::copy(budget, source, {}, [] { throw std::runtime_error("cancel"); })), std::runtime_error);
    EXPECT_EQ(budget->used(), 0U);
}

TEST(AdmittedMutationInputTest, CancellationDuringCopyFreesThePartiallyBuiltPrivateBatch) {
    using namespace graph::query::execution::detail;
    CopyCountingResource upstream;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(1048576U));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(1048576U));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_WRITE, 1048576U);
    auto budget = std::make_shared<MaterializedByteBudget>(1048576U, account, &upstream);
    LogicalGraphMutationBatch source; source.node_creates.resize(512U);
    bool copied = false;
    EXPECT_THROW(static_cast<void>(graph::storage::detail::AdmittedMutationInput::copy(budget, source, {}, [&] {
        if (upstream.allocations != 0U) { copied = true; throw std::runtime_error("cancel partial copy"); }
    })), std::runtime_error);
    EXPECT_TRUE(copied); EXPECT_EQ(budget->used(), 0U); EXPECT_EQ(account->used(), 0U);
    EXPECT_EQ(source.node_creates.size(), 512U);
}
