#include <gtest/gtest.h>
#include <array>
#include "graph/core/OwnedPropertyValue.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace {
using namespace graph;
using namespace query::execution;

TEST(OwnedPropertyValueTest, CopiesWithinOneOwnerShareImmutableDataAndReplaceIndependently) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    const PropertyValue source(std::vector<PropertyValue>(256U, PropertyValue(std::string(512U, 's'))));
    {
        utils::OwnedAllocationScope scope(memory);
        OwnedPropertyValue value(source);
        const auto retained = memory->used();
        OwnedPropertyValue copy(value);
        EXPECT_EQ(&value.get(), &copy.get());
        EXPECT_EQ(memory->used(), retained);
        copy = PropertyValue(int64_t{42});
        EXPECT_EQ(value.get(), source);
        EXPECT_EQ(copy.get(), PropertyValue(int64_t{42}));
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedPropertyValueTest, ScopeChangesClonePayloadBeforeReplacingItsOwner) {
    auto first = std::make_shared<MaterializedByteBudget>(65536U);
    auto second = std::make_shared<MaterializedByteBudget>(65536U);
    const PropertyValue source(std::string(8192U, 's'));
    std::optional<OwnedPropertyValue> value, copy;
    {
        utils::OwnedAllocationScope scope(first); value.emplace(source);
    }
    {
        utils::OwnedAllocationScope scope(second); copy.emplace(*value);
    }
    EXPECT_NE(&value->get(), &copy->get());
    EXPECT_GT(first->used(), 8192U); EXPECT_GT(second->used(), 8192U);
    value.reset(); EXPECT_EQ(first->used(), 0U);
    EXPECT_EQ(copy->get(), source);
    copy.reset(); EXPECT_EQ(second->used(), 0U);
}

TEST(OwnedPropertyValueTest, DeniedNestedReplacementPreservesTheOldLiteralAndCredit) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    const PropertyValue large(std::vector<PropertyValue>(256U, PropertyValue(std::string(512U, 's'))));
    {
        utils::OwnedAllocationScope scope(memory);
        OwnedPropertyValue value(PropertyValue(int64_t{7}));
        const auto retained = memory->used();
        EXPECT_THROW(value = large, MaterializedByteLimitExceeded);
        EXPECT_EQ(value.get(), PropertyValue(int64_t{7}));
        EXPECT_EQ(memory->used(), retained);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedPropertyValueTest, EmptyMovedOwnersRemainReusableAfterTheDestinationIsDestroyed) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    std::optional<OwnedPropertyValue> value;
    {
        utils::OwnedAllocationScope scope(memory); value.emplace(PropertyValue(std::string(8192U, 's')));
    }
    {
        OwnedPropertyValue moved(std::move(*value));
        EXPECT_EQ(moved.get(), PropertyValue(std::string(8192U, 's')));
        EXPECT_EQ(value->get().getType(), PropertyType::NULL_TYPE);
    }
    EXPECT_EQ(memory->used(), 0U);
    *value = PropertyValue(std::string(4096U, 'r'));
    EXPECT_GT(memory->used(), 4096U);
    value.reset(); EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedPropertyValueTest, LiteralConstructorDenialRefundsItsPartiallyAllocatedExpression) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    const PropertyValue source(std::vector<PropertyValue>(1024U, PropertyValue(int64_t{1})));
    {
        utils::OwnedAllocationScope scope(memory);
        EXPECT_THROW(static_cast<void>(std::make_unique<query::expressions::LiteralExpression>(source)),
            MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(source.getList().size(), 1024U);
}
} // namespace

TEST(OwnedPropertyListTest, WholeListAdmissionPrecedesCopyingAnyBorrowedScalarPayload) {
    using namespace graph;
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(4096U);
    const std::string text(8192U, 's');
    const auto source = [&](size_t) { return property_heap::CopySource(text); };
    {
        utils::OwnedAllocationScope scope(memory);
        EXPECT_THROW(static_cast<void>(OwnedPropertyValue::fromList(1024U, source)), query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(text.size(), 8192U);
}

TEST(OwnedPropertyListTest, BorrowedViewsPreserveMixedTypesAndEmbeddedNulls) {
    using namespace graph;
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(65536U);
    const std::string text("left\0right", 10U);
    const PropertyValue nested(std::vector<PropertyValue>{PropertyValue(int64_t{7})});
    const std::array<property_heap::CopySource, 5U> sources{
        property_heap::CopySource(true), property_heap::CopySource(int64_t{-2}),
        property_heap::CopySource(1.5), property_heap::CopySource(text), property_heap::CopySource(nested)};
    {
        utils::OwnedAllocationScope scope(memory);
        auto list = OwnedPropertyValue::fromList(sources.size(), [&](size_t index) { return sources[index]; });
        EXPECT_EQ(list.get(), PropertyValue(std::vector<PropertyValue>{PropertyValue(true), PropertyValue(int64_t{-2}),
            PropertyValue(1.5), PropertyValue(text), nested}));
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedPropertyListTest, FailureDuringTheCopyPassRefundsPartiallyConstructedElements) {
    using namespace graph;
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(65536U);
    const std::string text(8192U, 's');
    size_t calls = 0U;
    const auto source = [&](size_t) {
        if (++calls == 5U) throw std::runtime_error("cancel partial list construction");
        return property_heap::CopySource(text);
    };
    {
        utils::OwnedAllocationScope scope(memory);
        EXPECT_THROW(static_cast<void>(OwnedPropertyValue::fromList(3U, source)), std::runtime_error);
    }
    EXPECT_EQ(calls, 5U);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(text.size(), 8192U);
}
