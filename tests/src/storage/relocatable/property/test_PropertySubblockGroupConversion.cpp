#include <gtest/gtest.h>
#include <random>
#include "../../../../../benchmarks/architecture-design/property-subblock/ReservedGroupConversion.hpp"
using namespace zyx::benchmark::subblock;
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
namespace {
std::vector<PropertyCollectionRoot> rows(size_t owners, size_t fields = 1) {
    std::vector<PropertyCollectionRoot> result;
    for (size_t owner = 1; owner <= owners; ++owner) {
        PropertyCollectionRoot root{owner, graph::Node::typeId, {}};
        for (size_t token = 1; token <= fields; ++token)
            root.entries.push_back({uint32_t(token), PropertyValue(int64_t(owner * 100 + token))});
        result.push_back(std::move(root));
    }
    return result;
}
void checkRoundTrip(const ReservedGroupConversion &plan, const std::vector<PropertyCollectionRoot> &roots) {
    for (const auto &root : roots) {
        size_t count = 0;
        plan.visitOwner(root.owner_entity_id, [&](uint32_t token, const PropertyValue &value) {
            ASSERT_LT(count, root.entries.size());
            EXPECT_EQ(token, root.entries[count].key_token);
            // Canonical bytes also compare NaN payloads and signed zero exactly.
            std::vector<std::byte> before, after;
            CanonicalPropertyValueCodec::append(before, root.entries[count].value);
            CanonicalPropertyValueCodec::append(after, value);
            EXPECT_EQ(before, after); ++count;
        });
        EXPECT_EQ(count, root.entries.size());
    }
}
TEST(PropertySubblockGroupConversionTest, Exact47And48ThresholdAnd64UnorderedOwnersRoundTrip) {
    for (size_t count : {47U, 48U, 64U}) {
        auto source = rows(count); std::mt19937 random(734); std::shuffle(source.begin(), source.end(), random);
        MaterializedByteBudget budget(1024 * 1024);
        {
            ReservedGroupConversion plan(budget, source, OwnerKind::HPK_NODE, 1, 2, 3968);
            EXPECT_EQ(plan.columns().size(), count < 48 ? 0U : 1U);
            EXPECT_EQ(plan.members().size(), count); EXPECT_EQ(plan.group().memberships, count);
            EXPECT_EQ(plan.group().live, count == 64 ? UINT64_MAX : (uint64_t{1} << count) - 1);
            EXPECT_EQ(budget.used(), plan.retainedCharge());
            EXPECT_LE(plan.retainedCharge(), 1024U * 1024U); checkRoundTrip(plan, source);
        }
        EXPECT_EQ(budget.used(), 0U);
    }
}
TEST(PropertySubblockGroupConversionTest, DynamicMixedNullMissingAndLargeFieldsKeepExactCanonicalValues) {
    auto source = rows(64, 8);
    for (size_t i = 0; i < source.size(); ++i) {
        auto &entries = source[i].entries;
        entries[1].value = PropertyValue(double(i));
        entries[2].value = PropertyValue(bool(i % 2));
        entries[3].value = PropertyValue{}; // all NULL: cannot infer a new type
        entries[4].value = i == 0 ? PropertyValue(double(3)) : PropertyValue(int64_t(3));
        entries[5].value = PropertyValue(std::string(8192, char('a' + i % 26)));
        if (i < 16) entries[6].value = PropertyValue{}; // present NULL qualifies
        if (i >= 47) entries.erase(entries.begin() + 7); // sparse remains extension
    }
    source[0].entries[1].value = PropertyValue(std::bit_cast<double>(UINT64_C(0x7ff8000000000042)));
    source[1].entries[1].value = PropertyValue(-0.0);
    MaterializedByteBudget budget(1024 * 1024);
    ReservedGroupConversion plan(budget, source, OwnerKind::HPK_NODE, 1, 3, 3968);
    ASSERT_EQ(plan.columns().size(), 4U);
    EXPECT_EQ(plan.columns()[0].identity.token, 1U);
    EXPECT_EQ(plan.columns()[1].identity.token, 2U);
    EXPECT_EQ(plan.columns()[2].identity.token, 3U);
    EXPECT_EQ(plan.columns()[3].identity.token, 7U);
    EXPECT_EQ(plan.columns()[3].nulls, 65535U);
    for (const auto &member : plan.members())
        if (member.token == 4 || member.token == 5 || member.token == 6 || member.token == 8)
            EXPECT_EQ(member.source, Source::HPS_EXTENSION);
    checkRoundTrip(plan, source);
}
TEST(PropertySubblockGroupConversionTest, MaximumMembershipsAndPropertylessOwnersStayBounded) {
    auto source = rows(64, 64); MaterializedByteBudget budget(1024 * 1024);
    { ReservedGroupConversion plan(budget, source, OwnerKind::HPK_NODE, 1, 2, 3968);
      EXPECT_EQ(plan.members().size(), 4096U); EXPECT_EQ(plan.columns().size(), 64U); checkRoundTrip(plan, source); }
    source[0].entries.push_back({65, PropertyValue(int64_t(1))});
    EXPECT_THROW((ReservedGroupConversion(budget, source, OwnerKind::HPK_NODE, 1, 2, 3968)), std::length_error);
    EXPECT_EQ(budget.used(), 0U);
    source = rows(64, 0);
    ReservedGroupConversion empty(budget, source, OwnerKind::HPK_NODE, 1, 2, 64);
    EXPECT_TRUE(empty.members().empty()); EXPECT_TRUE(empty.columns().empty()); checkRoundTrip(empty, source);
}
TEST(PropertySubblockGroupConversionTest, CancellationAndAdmissionRejectWithoutChangingSourceOrLeakingCharge) {
    const auto source = rows(64); MaterializedByteBudget tiny(1), budget(1024 * 1024);
    EXPECT_THROW((ReservedGroupConversion(tiny, source, OwnerKind::HPK_NODE, 1, 2, 3968)), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny.used(), 0U);
    for (size_t stop : {1U, 32U, 65U, 66U}) {
        size_t seen = 0;
        EXPECT_THROW((ReservedGroupConversion(budget, source, OwnerKind::HPK_NODE, 1, 2, 3968,
            [&] { if (++seen == stop) throw std::runtime_error("cancel conversion"); })), std::runtime_error);
        EXPECT_EQ(budget.used(), 0U);
    }
    EXPECT_THROW((ReservedGroupConversion(budget, source, OwnerKind::HPK_NODE, 1, 2, 575)), std::length_error);
    EXPECT_EQ(budget.used(), 0U); EXPECT_EQ(source, rows(64));
    ReservedGroupConversion retry(budget, source, OwnerKind::HPK_NODE, 1, 2, 576); checkRoundTrip(retry, source);
}
TEST(PropertySubblockGroupConversionTest, RejectsWrongOwnerKindGroupDuplicateOwnerAndUnsortedTokens) {
    auto source = rows(64); MaterializedByteBudget budget(1024 * 1024);
    EXPECT_THROW((ReservedGroupConversion(budget, source, OwnerKind::HPK_EDGE, 1, 2, 3968)), std::invalid_argument);
    EXPECT_THROW((ReservedGroupConversion(budget, source, OwnerKind::HPK_NODE, 2, 2, 3968)), std::invalid_argument);
    source[1].owner_entity_id = 1;
    EXPECT_THROW((ReservedGroupConversion(budget, source, OwnerKind::HPK_NODE, 1, 2, 3968)), std::invalid_argument);
    source = rows(64); source[0].entries.push_back({1, PropertyValue(int64_t(2))});
    EXPECT_THROW((ReservedGroupConversion(budget, source, OwnerKind::HPK_NODE, 1, 2, 3968)), std::invalid_argument);
    EXPECT_EQ(budget.used(), 0U);
    auto edgeSource = rows(48); for (auto &root : edgeSource) root.owner_entity_type = graph::Edge::typeId;
    ReservedGroupConversion edgePlan(budget, edgeSource, OwnerKind::HPK_EDGE, 1, 2, 3968);
    EXPECT_EQ(edgePlan.columns()[0].identity.owner_kind, OwnerKind::HPK_EDGE); checkRoundTrip(edgePlan, edgeSource);
}
TEST(PropertySubblockGroupConversionTest, NativeOwnerLimitAndHighestPartialGroupDoNotExpandIdNamespace) {
    auto source = rows(1, 0); source[0].owner_entity_id = uint64_t(INT64_MAX);
    MaterializedByteBudget budget(1024 * 1024);
    { ReservedGroupConversion plan(budget, source, OwnerKind::HPK_NODE, groupFor(uint64_t(INT64_MAX)), 2, 64);
      checkRoundTrip(plan, source); EXPECT_EQ(plan.group().live, uint64_t{1} << 62);
      EXPECT_THROW(plan.visitOwner(uint64_t(INT64_MAX) - 1, [](auto, const auto &) {}), std::invalid_argument); }
    source[0].owner_entity_id = uint64_t(INT64_MAX) + 1;
    EXPECT_THROW((ReservedGroupConversion(budget, source, OwnerKind::HPK_NODE,
        groupFor(source[0].owner_entity_id), 2, 64)), std::invalid_argument);
    EXPECT_EQ(budget.used(), 0U);
}
TEST(PropertySubblockGroupConversionTest, ParentAccountOwnsThePlanAndFailureRefundsBothCounters) {
    using namespace graph::query::execution::detail;
    const auto makeAccount = [](size_t bytes) {
        auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(bytes));
        auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(bytes));
        return std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, bytes);
    };
    const auto source = rows(64);
    auto refused = makeAccount(1); MaterializedByteBudget denied(1024 * 1024, refused);
    EXPECT_THROW((ReservedGroupConversion(denied, source, OwnerKind::HPK_NODE, 1, 2, 3968)), ResourceAdmissionDenied);
    EXPECT_EQ(denied.used(), 0U); EXPECT_EQ(refused->used(), 0U);
    auto account = makeAccount(1024 * 1024); MaterializedByteBudget budget(1024 * 1024, account);
    { ReservedGroupConversion plan(budget, source, OwnerKind::HPK_NODE, 1, 2, 3968);
      EXPECT_EQ(plan.retainedCharge(), account->used()); EXPECT_EQ(budget.used(), account->used()); }
    EXPECT_EQ(budget.used(), 0U); EXPECT_EQ(account->used(), 0U);
    auto empty = rows(64, 0);
    EXPECT_THROW((ReservedGroupConversion(budget, empty, OwnerKind::HPK_NODE, 1, 2, 63)), std::length_error);
    EXPECT_EQ(account->used(), 0U);
}
} // namespace
