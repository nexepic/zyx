#include <gtest/gtest.h>
#include "graph/storage/relocatable/property/HybridPropertyGroupConversion.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
std::vector<PropertyCollectionRoot> nativeRows(size_t owners, size_t fields) {
    std::vector<PropertyCollectionRoot> result;
    for (size_t owner = 1; owner <= owners; ++owner) {
        PropertyCollectionRoot root;
        root.owner_entity_id = owner; root.owner_entity_type = graph::Node::typeId;
        for (size_t token = 1; token <= fields; ++token)
            root.entries.push_back({uint32_t(token), PropertyValue(int64_t(owner + token))});
        result.push_back(std::move(root));
    }
    return result;
}
using Plan = HybridPropertyGroupConversion;
constexpr auto NODE = EntityPropertyOwnerKind::EPOK_NODE;
TEST(HybridPropertyGroupConversionTest, RetainsTheExactAllocationOwnerUntilThePlanIsDestroyed) {
    const auto roots = nativeRows(48, 1);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    std::weak_ptr<MaterializedByteBudget> weak = memory;
    auto plan = std::make_unique<Plan>(memory, roots, NODE, 1, Generation(7), 3952);
    EXPECT_EQ(memory->used(), plan->retainedCharge());
    EXPECT_LE(plan->retainedCharge(), 1024U * 1024U);
    EXPECT_EQ(plan->members()[0].value, &roots[0].entries[0].value);
    memory.reset(); EXPECT_FALSE(weak.expired());
    EXPECT_EQ(plan->columns()[0].identity.semantic_generation, Generation(7));
    plan.reset(); EXPECT_TRUE(weak.expired());
}
TEST(HybridPropertyGroupConversionTest, MaximumColumnCountUsesNativePayloadsAndExactOwnerProjection) {
    const auto roots = nativeRows(48, 85);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, Generation(7), 3952);
        ASSERT_EQ(plan.columns().size(), 85U); ASSERT_EQ(plan.members().size(), 4080U);
        EXPECT_EQ(plan.group().columns, 85U); EXPECT_EQ(plan.group().memberships, 4080U);
        for (size_t i = 0; i < plan.columns().size(); ++i) {
            std::array<std::byte, 576> bytes{}; plan.encodeNativeColumn(i, bytes);
            const auto &column = plan.columns()[i];
            EXPECT_EQ(HybridPropertyColumnRecord::decode(bytes, column.identity, plan.group(),
                Generation(7), Generation(7)), column);
        }
        for (const auto &root : roots) {
            size_t seen = 0;
            plan.visitOwner(root.owner_entity_id, [&](uint32_t token, const PropertyValue &value) {
                ASSERT_LT(seen, root.entries.size());
                EXPECT_EQ(token, root.entries[seen].key_token); EXPECT_EQ(value, root.entries[seen].value); ++seen;
            });
            EXPECT_EQ(seen, root.entries.size());
        }
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyGroupConversionTest, QuotaCancellationAndShapeRefusalsRefundBeforeRetry) {
    const auto roots = nativeRows(64, 1);
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW((Plan(tiny, roots, NODE, 1, Generation(7), 3952)),
        graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    size_t calls = 0;
    EXPECT_THROW((Plan(memory, roots, NODE, 1, Generation(7), 3952,
        [&] { if (++calls == 65) throw std::runtime_error("cancel native conversion"); })), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW((Plan(memory, roots, NODE, 1, Generation(7), 575)), std::length_error);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW((Plan({}, roots, NODE, 1, Generation(7), 3952)), std::invalid_argument);
    EXPECT_EQ(roots, nativeRows(64, 1));
    { Plan retry(memory, roots, NODE, 1, Generation(7), 576); EXPECT_EQ(retry.columns().size(), 1U); }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyGroupConversionTest, NullMixedAndHeapExtensionsRemainBorrowedAndOrdered) {
    auto roots = nativeRows(64, 4);
    for (size_t i = 0; i < roots.size(); ++i) {
        roots[i].entries[1].value = PropertyValue{};
        roots[i].entries[2].value = i == 0 ? PropertyValue(1.0) : PropertyValue(int64_t(1));
        roots[i].entries[3].value = PropertyValue(std::string(8192, 'x'));
    }
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, Generation(7), 3952);
        ASSERT_EQ(plan.columns().size(), 1U);
        for (const auto &member : plan.members()) {
            EXPECT_EQ(member.source, member.token == 1 ? HybridPropertySource::HPS_COLUMN : HybridPropertySource::HPS_EXTENSION);
            EXPECT_EQ(member.value, &roots[member.owner - 1].entries[member.token - 1].value);
        }
        size_t seen = 0;
        plan.visitOwner(1, [&](uint32_t token, const PropertyValue &value) {
            EXPECT_EQ(token, ++seen); EXPECT_EQ(value, roots[0].entries[token - 1].value);
        });
        EXPECT_EQ(seen, 4U); EXPECT_EQ(memory->used(), plan.retainedCharge());
    }
    EXPECT_EQ(memory->used(), 0U);
}
} // namespace

TEST(HybridPropertyGroupConversionTest, DenseShortTextUsesExistingBoundedColumnsAndLongTextRetainsExtension) {
 auto roots=nativeRows(64,2);auto memory=std::make_shared<MaterializedByteBudget>(1024*1024);
 for(size_t i=0;i<roots.size();++i) {
  roots[i].entries[0].value=i==4?PropertyValue{}:PropertyValue(i%2?std::string("a\0b",3):std::string("CN"));
  roots[i].entries[1].value=PropertyValue(std::string(8,'x'));
 }
 {
  Plan plan(memory,roots,NODE,1,Generation(7),3952);
  ASSERT_EQ(plan.columns().size(),1U);
  EXPECT_EQ(plan.columns()[0].identity.scalar_kind,HybridPropertyScalarKind::HPSK_SHORT_TEXT7);
  for(const auto &root:roots) {
   std::vector<PropertyCollectionRootEntry> selected;
   plan.visitOwner(root.owner_entity_id,[&](auto token,const auto &value){selected.push_back({token,value});});
   EXPECT_EQ(selected,root.entries);
  }
 }
 EXPECT_EQ(memory->used(),0U);
 roots[0].entries[0].value=PropertyValue(std::string(8,'y'));
 {
  Plan plan(memory,roots,NODE,1,Generation(7),3952);
  EXPECT_TRUE(plan.columns().empty());
 }
 EXPECT_EQ(memory->used(),0U);
}
