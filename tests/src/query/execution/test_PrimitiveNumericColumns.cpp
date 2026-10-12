#include <gtest/gtest.h>

#include "query/execution/PrimitiveNumericColumns.hpp"

namespace graph::query::execution {
namespace {

class NumericFaultResource final : public std::pmr::memory_resource {
public:
	size_t calls = 0U;
	size_t fail_at = std::numeric_limits<size_t>::max();
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (++calls == fail_at) throw std::bad_alloc();
		return std::pmr::get_default_resource()->allocate(bytes, alignment);
	}
	void do_deallocate(void *value, size_t bytes, size_t alignment) override {
		std::pmr::get_default_resource()->deallocate(value, bytes, alignment);
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

TEST(PrimitiveNumericColumnsTest, ConvertsUniqueIntegerFieldsAndPreservesNullCountSumAndAverage) {
	const std::vector<std::vector<PropertyValue>> source{{PropertyValue(2), PropertyValue{}, PropertyValue(4)}};
	const std::vector<size_t> requested{0U, 0U, std::numeric_limits<size_t>::max()};
	MaterializedByteBudget budget(65536U);
	{
		detail::PrimitiveNumericColumns columns(budget);
		ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
		EXPECT_EQ(columns.kind(0U), detail::NumericColumnKind::NCK_INTEGER);
		EXPECT_EQ(columns.typedColumnCount(), 1U);
		CountAccumulator count;
		SumAccumulator sum;
		AvgAccumulator average;
		for (size_t row = 0U; row < 3U; ++row) {
			columns.update(count, AggregateFunctionType::AGG_COUNT, 0U, row);
			columns.update(sum, AggregateFunctionType::AGG_SUM, 0U, row);
			columns.update(average, AggregateFunctionType::AGG_AVG, 0U, row);
		}
		EXPECT_EQ(count.getResult(), PropertyValue(2));
		EXPECT_EQ(sum.getResult(), PropertyValue(6));
		EXPECT_EQ(average.getResult(), PropertyValue(3.0));
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(PrimitiveNumericColumnsTest, FloatingBuffersRetainNanAndInfinityAndCallerSpecifiedOrder) {
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const std::vector<std::vector<PropertyValue>> source{
		{PropertyValue(1e16), PropertyValue(-1e16), PropertyValue(1.0)},
		{PropertyValue(nan), PropertyValue(std::numeric_limits<double>::infinity()), PropertyValue(-0.0)}};
	const std::vector<size_t> requested{0U, 1U};
	MaterializedByteBudget budget(65536U);
	detail::PrimitiveNumericColumns columns(budget);
	ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	SumAccumulator sum;
	for (size_t row : {2U, 0U, 1U}) columns.update(sum, AggregateFunctionType::AGG_SUM, 0U, row);
	EXPECT_EQ(sum.getResult(), PropertyValue(0.0));
	SumAccumulator special;
	columns.update(special, AggregateFunctionType::AGG_SUM, 1U, 1U);
	EXPECT_TRUE(std::isinf(std::get<double>(special.getResult().getVariant())));
	columns.update(special, AggregateFunctionType::AGG_SUM, 1U, 0U);
	EXPECT_TRUE(std::isnan(std::get<double>(special.getResult().getVariant())));
}

TEST(PrimitiveNumericColumnsTest, MixedNumericAndInvalidValuesUseTheExistingDynamicAccumulator) {
	const std::vector<std::vector<PropertyValue>> source{{PropertyValue(2), PropertyValue(0.5)}, {PropertyValue(true), PropertyValue{}}};
	const std::vector<size_t> requested{0U, 1U};
	MaterializedByteBudget budget(65536U);
	detail::PrimitiveNumericColumns columns(budget);
	ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	EXPECT_EQ(columns.typedColumnCount(), 0U);
	SumAccumulator sum;
	columns.update(sum, AggregateFunctionType::AGG_SUM, 0U, 0U);
	columns.update(sum, AggregateFunctionType::AGG_SUM, 0U, 1U);
	EXPECT_EQ(sum.getResult(), PropertyValue(2.5));
	EXPECT_THROW(columns.update(sum, AggregateFunctionType::AGG_SUM, 1U, 0U), std::invalid_argument);
	EXPECT_EQ(sum.getResult(), PropertyValue(2.5));
}

TEST(PrimitiveNumericColumnsTest, NullOnlyFieldsNeedNoValueBuffersAndValidateAccessBeforeSkipping) {
	const std::vector<std::vector<PropertyValue>> source{{PropertyValue{}, PropertyValue{}}};
	const std::vector<size_t> requested{0U};
	MaterializedByteBudget budget(65536U);
	detail::PrimitiveNumericColumns columns(budget);
	ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	EXPECT_EQ(columns.kind(0U), detail::NumericColumnKind::NCK_NULL);
	SumAccumulator sum;
	columns.update(sum, AggregateFunctionType::AGG_SUM, 0U, 0U);
	EXPECT_EQ(sum.getResult().getType(), PropertyType::NULL_TYPE);
	EXPECT_THROW(columns.update(sum, AggregateFunctionType::AGG_SUM, 0U, 2U), std::out_of_range);
	EXPECT_THROW(columns.update(sum, AggregateFunctionType::AGG_COLLECT, 0U, 0U), std::invalid_argument);
}

TEST(PrimitiveNumericColumnsTest, MetadataAndPartialBufferBudgetFailuresDeclineWithoutChangingAggregates) {
	const std::vector<std::vector<PropertyValue>> source{std::vector<PropertyValue>(128U, PropertyValue(1))};
	const std::vector<size_t> requested{0U};
	MaterializedByteBudget tiny(1U);
	detail::PrimitiveNumericColumns declined(tiny);
	EXPECT_FALSE(declined.tryPrepare(source, requested, [] {}));
	EXPECT_EQ(tiny.used(), 0U);
	MaterializedByteBudget budget(768U);
	{
		detail::PrimitiveNumericColumns columns(budget);
		ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
		EXPECT_EQ(columns.kind(0U), detail::NumericColumnKind::NCK_DYNAMIC);
		SumAccumulator sum;
		columns.update(sum, AggregateFunctionType::AGG_SUM, 0U, 0U);
		EXPECT_EQ(sum.getResult(), PropertyValue(1));
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(PrimitiveNumericColumnsTest, EveryAllocatorFailureEscapesAndOwnedCapacityIsRefunded) {
	const std::vector<std::vector<PropertyValue>> source{{PropertyValue(1), PropertyValue(2)}, {PropertyValue(1.0), PropertyValue(2.0)}};
	const std::vector<size_t> requested{0U, 1U};
	NumericFaultResource probe;
	MaterializedByteBudget budget(65536U);
	{
		detail::PrimitiveNumericColumns columns(budget, &probe);
		ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	}
	EXPECT_EQ(budget.used(), 0U);
	for (size_t fault = 1U; fault <= probe.calls; ++fault) {
		NumericFaultResource memory;
		memory.fail_at = fault;
		{
			detail::PrimitiveNumericColumns columns(budget, &memory);
			EXPECT_THROW(static_cast<void>(columns.tryPrepare(source, requested, [] {})), std::bad_alloc);
		}
		EXPECT_EQ(budget.used(), 0U);
	}
}

TEST(PrimitiveNumericColumnsTest, CancellationIncludingBudgetExceptionsIsNotMistakenForOptionalAdmissionFailure) {
	const std::vector<std::vector<PropertyValue>> source{{PropertyValue(1), PropertyValue(2), PropertyValue(3)},
		{PropertyValue(1.0), PropertyValue(2.0), PropertyValue(3.0)}};
	const std::vector<size_t> requested{0U, 1U};
	MaterializedByteBudget budget(65536U);
	for (size_t stop : {1U, 5U, 10U}) {
		size_t calls = 0U;
		{
			detail::PrimitiveNumericColumns columns(budget);
			EXPECT_THROW(static_cast<void>(columns.tryPrepare(source, requested, [&] {
				if (++calls == stop) throw MaterializedByteLimitExceeded();
			})), MaterializedByteLimitExceeded);
		}
		EXPECT_EQ(budget.used(), 0U);
	}
}

TEST(PrimitiveNumericColumnsTest, InvalidShapesAndBindingsFailBeforeAllocating) {
	MaterializedByteBudget budget(65536U);
	detail::PrimitiveNumericColumns columns(budget);
	const std::vector<std::vector<PropertyValue>> source{{PropertyValue(1)}, {}};
	const std::vector<size_t> valid{0U};
	EXPECT_THROW(static_cast<void>(columns.tryPrepare(source, valid, [] {})), std::invalid_argument);
	const std::vector<std::vector<PropertyValue>> rectangular{{PropertyValue(1)}};
	const std::vector<size_t> invalid{1U};
	EXPECT_THROW(static_cast<void>(columns.tryPrepare(rectangular, invalid, [] {})), std::out_of_range);
	EXPECT_EQ(budget.used(), 0U);
}

TEST(PrimitiveNumericColumnsTest, TypedExtremaPreserveNanAndExistingCrossTypeOrdering) {
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const std::vector<std::vector<PropertyValue>> source{{PropertyValue(2), PropertyValue(1)},
		{PropertyValue(0.5), PropertyValue(0.25)}, {PropertyValue(nan), PropertyValue(1.0)}};
	const std::vector<size_t> requested{0U, 1U, 2U};
	MaterializedByteBudget budget(65536U);
	detail::PrimitiveNumericColumns columns(budget);
	ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	MinAccumulator minimum;
	MaxAccumulator maximum;
	for (size_t column : {0U, 1U}) for (size_t row : {0U, 1U}) {
		columns.update(minimum, AggregateFunctionType::AGG_MIN, column, row);
		columns.update(maximum, AggregateFunctionType::AGG_MAX, column, row);
	}
	EXPECT_EQ(minimum.getResult(), PropertyValue(1));
	EXPECT_EQ(maximum.getResult(), PropertyValue(0.5));
	MinAccumulator floatingMinimum;
	MaxAccumulator floatingMaximum;
	columns.update(floatingMinimum, AggregateFunctionType::AGG_MIN, 1U, 0U);
	columns.update(floatingMinimum, AggregateFunctionType::AGG_MIN, 1U, 1U);
	columns.update(floatingMaximum, AggregateFunctionType::AGG_MAX, 1U, 1U);
	columns.update(floatingMaximum, AggregateFunctionType::AGG_MAX, 1U, 0U);
	EXPECT_EQ(floatingMinimum.getResult(), PropertyValue(0.25));
	EXPECT_EQ(floatingMaximum.getResult(), PropertyValue(0.5));
	MinAccumulator nanMinimum;
	MaxAccumulator nanMaximum;
	for (size_t row : {0U, 1U}) {
		columns.update(nanMinimum, AggregateFunctionType::AGG_MIN, 2U, row);
		columns.update(nanMaximum, AggregateFunctionType::AGG_MAX, 2U, row);
	}
	EXPECT_TRUE(std::isnan(std::get<double>(nanMinimum.getResult().getVariant())));
	EXPECT_TRUE(std::isnan(std::get<double>(nanMaximum.getResult().getVariant())));
}

TEST(PrimitiveNumericColumnsTest, DynamicExtremumAdmissionFailureKeepsTheExistingOwningValue) {
	const std::vector<std::vector<PropertyValue>> source{{PropertyValue(std::string(8192U, 'a'))}};
	const std::vector<size_t> requested{0U};
	MaterializedByteBudget budget(4096U);
	detail::PrimitiveNumericColumns columns(budget);
	ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	MinAccumulator minimum;
	minimum.update(PropertyValue("m"));
	const size_t prior = budget.used();
	{
		MaterializedByteReservation update(&budget);
		EXPECT_THROW(columns.update(minimum, AggregateFunctionType::AGG_MIN, 0U, 0U,
			[&](size_t bytes) { update.set(bytes); }), MaterializedByteLimitExceeded);
	}
	EXPECT_EQ(minimum.getResult(), PropertyValue("m"));
	EXPECT_EQ(budget.used(), prior);
}

TEST(PrimitiveNumericColumnsTest, ReleaseRefundsOnlyOwnedBuffersAndAllowsFreshPreparation) {
	const std::vector<std::vector<PropertyValue>> source{{PropertyValue(1), PropertyValue(2)}};
	const std::vector<size_t> requested{0U};
	MaterializedByteBudget budget(65536U);
	MaterializedByteReservation otherOwner(&budget);
	otherOwner.set(256U);
	detail::PrimitiveNumericColumns columns(budget);
	ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	EXPECT_GT(budget.used(), 256U);
	columns.release();
	EXPECT_EQ(budget.used(), 256U);
	EXPECT_EQ(columns.typedColumnCount(), 0U);
	EXPECT_THROW(static_cast<void>(columns.kind(0U)), std::out_of_range);
	ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	SumAccumulator sum;
	columns.update(sum, AggregateFunctionType::AGG_SUM, 0U, 1U);
	EXPECT_EQ(sum.getResult(), PropertyValue(2));
	columns.release();
	EXPECT_EQ(budget.used(), 256U);
}

} // namespace
TEST(PrimitiveNumericColumnsTest, TypedDeviationMatchesScalarWelfordOrderAndNullSemantics) {
	const std::vector<std::vector<PropertyValue>> source{
		{PropertyValue(int64_t{9007199254740993LL}), PropertyValue{}, PropertyValue(int64_t{9007199254740995LL})},
		{PropertyValue(1e16), PropertyValue(-1e16), PropertyValue(1.0)},
		{PropertyValue(2), PropertyValue(0.5), PropertyValue{}},
		{PropertyValue{}, PropertyValue{}, PropertyValue{}}};
	const std::vector<size_t> requested{0U, 1U, 2U, 3U};
	MaterializedByteBudget budget(65536U);
	detail::PrimitiveNumericColumns columns(budget);
	ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	for (bool population : {false, true}) {
		const auto function = population ? AggregateFunctionType::AGG_STDEVP : AggregateFunctionType::AGG_STDEV;
		for (size_t column = 0U; column < source.size(); ++column) {
			StDevAccumulator typed(population), scalar(population);
			for (size_t row : {2U, 0U, 1U}) {
				columns.update(typed, function, column, row);
				scalar.update(source[column][row]);
			}
			EXPECT_EQ(typed.getResult(), scalar.getResult());
		}
	}
}

TEST(PrimitiveNumericColumnsTest, DeviationPropagatesNonFiniteValuesAndRejectsDynamicInvalidInputs) {
	const std::vector<std::vector<PropertyValue>> source{
		{PropertyValue(std::numeric_limits<double>::infinity()), PropertyValue(1.0)},
		{PropertyValue("invalid"), PropertyValue{}}};
	const std::vector<size_t> requested{0U, 1U};
	MaterializedByteBudget budget(65536U);
	detail::PrimitiveNumericColumns columns(budget);
	ASSERT_TRUE(columns.tryPrepare(source, requested, [] {}));
	for (bool population : {false, true}) {
		StDevAccumulator deviation(population);
		const auto function = population ? AggregateFunctionType::AGG_STDEVP : AggregateFunctionType::AGG_STDEV;
		columns.update(deviation, function, 0U, 0U);
		columns.update(deviation, function, 0U, 1U);
		EXPECT_TRUE(std::isnan(std::get<double>(deviation.getResult().getVariant())));
		EXPECT_THROW(columns.update(deviation, function, 1U, 0U), std::invalid_argument);
	}
}

} // namespace graph::query::execution
