#include <gtest/gtest.h>

#include "query/execution/PrimitiveColumnFilter.hpp"

namespace graph::query::execution {
namespace {
using detail::ColumnPredicate;
using detail::ColumnPredicateOp;

constexpr ColumnPredicateOp comparisons[] = {
	ColumnPredicateOp::CPO_EQUAL, ColumnPredicateOp::CPO_NOT_EQUAL,
	ColumnPredicateOp::CPO_LESS, ColumnPredicateOp::CPO_GREATER,
	ColumnPredicateOp::CPO_LESS_EQUAL, ColumnPredicateOp::CPO_GREATER_EQUAL
};

bool scalarComparison(const PropertyValue &left, const PropertyValue &right, ColumnPredicateOp op) {
	if (left.getType() == PropertyType::NULL_TYPE || right.getType() == PropertyType::NULL_TYPE) return false;
	switch (op) {
		case ColumnPredicateOp::CPO_EQUAL: return left == right;
		case ColumnPredicateOp::CPO_NOT_EQUAL: return left != right;
		case ColumnPredicateOp::CPO_LESS: return left < right;
		case ColumnPredicateOp::CPO_GREATER: return left > right;
		case ColumnPredicateOp::CPO_LESS_EQUAL: return left <= right;
		case ColumnPredicateOp::CPO_GREATER_EQUAL: return left >= right;
		default: throw std::logic_error("reference comparison only accepts binary comparisons");
	}
}

void compareAll(const std::vector<PropertyValue> &values, const PropertyValue &expected) {
	for (const auto operation : comparisons) {
		MaterializedByteBudget budget(65536U);
		SelectionVector selection(values.size());
		const std::vector<ColumnPredicate> conditions{{0U, operation, &expected}};
		ASSERT_TRUE(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [] {}));
		for (size_t index = 0U; index < values.size(); ++index)
			EXPECT_EQ(selection.includes(index), scalarComparison(values[index], expected, operation));
		EXPECT_EQ(budget.used(), 0U);
	}
}

TEST(PrimitiveColumnFilterTest, IntegerComparisonsPreserveFullPrecisionAndNullSemantics) {
	compareAll({PropertyValue((std::numeric_limits<int64_t>::min)()), PropertyValue(int64_t{0}),
		PropertyValue(int64_t{9007199254740993LL}), PropertyValue((std::numeric_limits<int64_t>::max)()), PropertyValue{}},
		PropertyValue(int64_t{9007199254740992LL}));
}

TEST(PrimitiveColumnFilterTest, FloatingComparisonsPreserveNanInfinityAndSignedZeroOrdering) {
	compareAll({PropertyValue(-std::numeric_limits<double>::infinity()), PropertyValue(-0.0),
		PropertyValue(0.0), PropertyValue(1.0), PropertyValue(std::numeric_limits<double>::infinity()),
		PropertyValue(std::numeric_limits<double>::quiet_NaN()), PropertyValue{}}, PropertyValue(0.0));
	compareAll({PropertyValue(1.0), PropertyValue(std::numeric_limits<double>::quiet_NaN())},
		PropertyValue(std::numeric_limits<double>::quiet_NaN()));
}

TEST(PrimitiveColumnFilterTest, BooleanComparisonsUseContiguousByteValues) {
	compareAll({PropertyValue(false), PropertyValue(true), PropertyValue{}}, PropertyValue(true));
}

TEST(PrimitiveColumnFilterTest, MultipleConditionsIntersectWithoutReselectingEarlierRejectedRows) {
	const std::vector<PropertyValue> values{PropertyValue(1), PropertyValue(2), PropertyValue(3), PropertyValue(4), PropertyValue{}};
	const PropertyValue lower(2), upper(4);
	const std::vector<ColumnPredicate> conditions{{0U, ColumnPredicateOp::CPO_GREATER_EQUAL, &lower},
		{0U, ColumnPredicateOp::CPO_LESS, &upper}};
	MaterializedByteBudget budget(4096U);
	SelectionVector selection(values.size());
	selection.reject(2U);
	ASSERT_TRUE(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [] {}));
	EXPECT_EQ(selection.selectedCount(), 1U);
	EXPECT_TRUE(selection.includes(1U));
	EXPECT_FALSE(selection.includes(2U));
}

TEST(PrimitiveColumnFilterTest, MixedTypesAndCrossTypeConstantsDeclineWithoutChangingSelection) {
	for (const std::vector<PropertyValue> &values : {
		std::vector<PropertyValue>{PropertyValue(1), PropertyValue(1.0)},
		std::vector<PropertyValue>{PropertyValue(1), PropertyValue(true)},
		std::vector<PropertyValue>{PropertyValue("x"), PropertyValue{}}}) {
		MaterializedByteBudget budget(4096U);
		SelectionVector selection(values.size());
		const PropertyValue expected(1);
		const std::vector<ColumnPredicate> conditions{{0U, ColumnPredicateOp::CPO_EQUAL, &expected}};
		EXPECT_FALSE(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [] {}));
		EXPECT_TRUE(selection.allSelected());
		EXPECT_EQ(budget.used(), 0U);
	}
	const std::vector<PropertyValue> values{PropertyValue(1), PropertyValue(2)};
	const PropertyValue expected(1.0);
	const std::vector<ColumnPredicate> conditions{{0U, ColumnPredicateOp::CPO_EQUAL, &expected}};
	MaterializedByteBudget budget(4096U);
	SelectionVector selection(values.size());
	EXPECT_FALSE(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [] {}));
	EXPECT_TRUE(selection.allSelected());
}

TEST(PrimitiveColumnFilterTest, EmptyPredicatesAndAllNullColumnsNeedNoTypedAllocation) {
	MaterializedByteBudget budget(1U);
	const std::vector<PropertyValue> values{PropertyValue{}, PropertyValue{}};
	SelectionVector selection(values.size());
	EXPECT_TRUE(detail::tryFilterPrimitiveColumn(values, {}, selection, budget, [] {}));
	EXPECT_TRUE(selection.allSelected());
	const PropertyValue expected(1);
	const std::vector<ColumnPredicate> conditions{{0U, ColumnPredicateOp::CPO_EQUAL, &expected}};
	EXPECT_TRUE(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [] {}));
	EXPECT_TRUE(selection.noneSelected());
	EXPECT_EQ(budget.used(), 0U);
	SelectionVector empty(0U);
	EXPECT_TRUE(detail::tryFilterPrimitiveColumn({}, conditions, empty, budget, [] {}));
}

TEST(PrimitiveColumnFilterTest, NonNullConditionWorksWithoutAnExpectedValue) {
	const std::vector<PropertyValue> values{PropertyValue(false), PropertyValue{}, PropertyValue(true)};
	const std::vector<ColumnPredicate> conditions{{0U, ColumnPredicateOp::CPO_NON_NULL, nullptr}};
	MaterializedByteBudget budget(4096U);
	SelectionVector selection(values.size());
	ASSERT_TRUE(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [] {}));
	EXPECT_EQ(selection.selectedCount(), 2U);
	EXPECT_FALSE(selection.includes(1U));
}

TEST(PrimitiveColumnFilterTest, SharedBudgetPressureDeclinesAndKeepsOtherOwnersCharged) {
	const std::vector<PropertyValue> values{PropertyValue(1), PropertyValue(2)};
	const PropertyValue expected(1);
	const std::vector<ColumnPredicate> conditions{{0U, ColumnPredicateOp::CPO_EQUAL, &expected}};
	MaterializedByteBudget budget(128U);
	MaterializedByteReservation other(&budget);
	other.set(127U);
	SelectionVector selection(values.size());
	EXPECT_FALSE(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [] {}));
	EXPECT_TRUE(selection.allSelected());
	EXPECT_EQ(budget.used(), 127U);
	other.reset();
	EXPECT_TRUE(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [] {}));
	EXPECT_EQ(selection.selectedCount(), 1U);
	EXPECT_EQ(budget.used(), 0U);
}

class FailingFilterResource final : public std::pmr::memory_resource {
public:
	size_t requests = 0U;
	size_t fail_at = (std::numeric_limits<size_t>::max)();
	size_t live = 0U;
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (requests++ == fail_at) throw std::bad_alloc();
		void *result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
		live += bytes;
		return result;
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
		live -= bytes;
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

TEST(PrimitiveColumnFilterTest, AllocationFailuresPropagateWithoutPublishingSelectionAndRefundCapacity) {
	const std::vector<PropertyValue> values{PropertyValue(1), PropertyValue(2)};
	const PropertyValue expected(1);
	const std::vector<ColumnPredicate> conditions{{0U, ColumnPredicateOp::CPO_EQUAL, &expected}};
	FailingFilterResource resource;
	MaterializedByteBudget budget(4096U);
	SelectionVector first(values.size());
	ASSERT_TRUE(detail::tryFilterPrimitiveColumn(values, conditions, first, budget, [] {}, &resource));
	const size_t allocations = resource.requests;
	for (size_t failure = 0U; failure < allocations; ++failure) {
		resource.requests = 0U;
		resource.fail_at = failure;
		SelectionVector selection(values.size());
		EXPECT_THROW(static_cast<void>(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [] {}, &resource)), std::bad_alloc);
		EXPECT_TRUE(selection.allSelected());
		EXPECT_EQ(resource.live, 0U);
		EXPECT_EQ(budget.used(), 0U);
	}
}

TEST(PrimitiveColumnFilterTest, CancellationEscapesDuringPreparationAndFilteringAndRefundsOwnedMemory) {
	const std::vector<PropertyValue> values(8U, PropertyValue(1));
	const PropertyValue expected(0);
	const std::vector<ColumnPredicate> conditions{{0U, ColumnPredicateOp::CPO_EQUAL, &expected}};
	for (const size_t cancelAt : {1U, 5U, 12U, 20U}) {
		MaterializedByteBudget budget(4096U);
		SelectionVector selection(values.size());
		size_t calls = 0U;
		EXPECT_THROW(static_cast<void>(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [&] {
			if (++calls == cancelAt) throw std::runtime_error("query canceled");
		})), std::runtime_error);
		EXPECT_EQ(budget.used(), 0U);
	}
	MaterializedByteBudget budget(4096U);
	SelectionVector selection(values.size());
	size_t calls = 0U;
	EXPECT_THROW(static_cast<void>(detail::tryFilterPrimitiveColumn(values, conditions, selection, budget, [&] {
		if (++calls == 12U) throw MaterializedByteLimitExceeded();
	})), MaterializedByteLimitExceeded);
	EXPECT_EQ(budget.used(), 0U);
}

TEST(PrimitiveColumnFilterTest, InvalidDescriptorsAndShapeFailBeforeChangingSelection) {
	const std::vector<PropertyValue> values{PropertyValue(1)};
	MaterializedByteBudget budget(4096U);
	SelectionVector wrong(2U), selection(1U);
	const PropertyValue expected(1);
	const std::vector<ColumnPredicate> valid{{0U, ColumnPredicateOp::CPO_EQUAL, &expected}};
	const std::vector<ColumnPredicate> missing{{0U, ColumnPredicateOp::CPO_EQUAL, nullptr}};
	const std::vector<ColumnPredicate> invalid{{0U, static_cast<ColumnPredicateOp>(255), &expected}};
	EXPECT_THROW(static_cast<void>(detail::tryFilterPrimitiveColumn(values, valid, wrong, budget, [] {})), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(detail::tryFilterPrimitiveColumn(values, missing, selection, budget, [] {})), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(detail::tryFilterPrimitiveColumn(values, invalid, selection, budget, [] {})), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(detail::tryFilterPrimitiveColumn(values, valid, selection, budget, [] {}, nullptr)), std::invalid_argument);
	EXPECT_TRUE(selection.allSelected());
	EXPECT_EQ(budget.used(), 0U);
}

} // namespace
} // namespace graph::query::execution
