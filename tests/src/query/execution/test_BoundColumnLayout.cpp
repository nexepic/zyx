#include <gtest/gtest.h>
#include <optional>

#include "query/execution/BoundColumnLayout.hpp"

namespace graph::query::execution::detail {
namespace {
class LayoutAllocator final : public std::pmr::memory_resource {
public:
	size_t calls = 0U, live = 0U;
	size_t fail_at = (std::numeric_limits<size_t>::max)();
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (calls++ == fail_at) throw std::bad_alloc();
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

TEST(BoundColumnLayoutTest, OwnsOrderedNamesAfterProducerStringsChangeAndPreservesDescriptors) {
	MaterializedByteBudget budget(65536U);
	std::vector<std::string> inputs{"rank", std::string(256U, 'p')};
	BoundColumnLayout layout(inputs, budget);
	const auto rank = layout.column(0U), payload = layout.column(1U);
	EXPECT_EQ(layout.find("rank"), rank);
	EXPECT_FALSE(layout.find("absent"));
	inputs[0] = "changed";
	inputs.clear();
	inputs.shrink_to_fit();
	EXPECT_EQ(layout.name(rank), "rank");
	EXPECT_EQ(layout.name(payload), std::string(256U, 'p'));
	EXPECT_NE(rank.index, payload.index);
	EXPECT_EQ(rank.layout, payload.layout);
}

TEST(BoundColumnLayoutTest, ReorderedRenamedMissingAndAdditionalColumnsFailBeforeConsumerEffects) {
	MaterializedByteBudget budget(65536U);
	const std::vector<std::string> original{"a", "b"};
	BoundColumnLayout layout(original, budget);
	size_t effects = 0U;
	const auto consume = [&](const auto &names) { layout.validate(names); ++effects; };
	consume(original);
	for (const std::vector<std::string> &changed : {std::vector<std::string>{"b", "a"},
		std::vector<std::string>{"a", "c"}, std::vector<std::string>{"a"}, std::vector<std::string>{"a", "b", "c"}}) {
		EXPECT_THROW(consume(changed), std::logic_error);
		EXPECT_EQ(effects, 1U);
	}
	consume(original);
	EXPECT_EQ(effects, 2U);
	EXPECT_NO_THROW(validateColumnBinding(original, 1U, "b"));
	EXPECT_THROW(validateColumnBinding(original, 0U, "b"), std::logic_error);
	EXPECT_THROW(validateColumnBinding(original, 2U, "b"), std::logic_error);
}

TEST(BoundColumnLayoutTest, ForeignZeroAndReconstructedLayoutsRejectDescriptors) {
	MaterializedByteBudget budget(65536U);
	const std::vector<std::string> names{"rank"};
	std::optional<BoundColumnLayout> first;
	first.emplace(names, budget);
	const auto old = first->column(0U);
	const void *address = &*first;
	BoundColumnLayout other(names, budget);
	EXPECT_THROW(static_cast<void>(other.name(old)), std::invalid_argument);
	first.reset();
	first.emplace(names, budget);
	EXPECT_EQ(address, &*first);
	EXPECT_THROW(static_cast<void>(first->name(old)), std::invalid_argument);
	auto invalid = first->column(0U);
	invalid.layout = 0U;
	EXPECT_THROW(static_cast<void>(first->name(invalid)), std::invalid_argument);
	invalid = first->column(0U);
	invalid.index = 1U;
	EXPECT_THROW(static_cast<void>(first->name(invalid)), std::out_of_range);
	EXPECT_THROW(static_cast<void>(first->column(1U)), std::out_of_range);
}

TEST(BoundColumnLayoutTest, InvalidNamesRefundConstructionAndEmptyLayoutsAreValid) {
	MaterializedByteBudget budget(65536U);
	for (const std::vector<std::string> &invalid : {std::vector<std::string>{""}, std::vector<std::string>{"a", "a"}}) {
		EXPECT_THROW((BoundColumnLayout(invalid, budget)), std::invalid_argument);
		EXPECT_EQ(budget.used(), 0U);
	}
	const std::vector<std::string> names{"a"};
	EXPECT_THROW((BoundColumnLayout(names, budget, nullptr)), std::invalid_argument);
	EXPECT_EQ(budget.used(), 0U);
	BoundColumnLayout empty({}, budget);
	EXPECT_EQ(empty.size(), 0U);
	EXPECT_NO_THROW(empty.validate({}));
	EXPECT_FALSE(empty.find("a"));
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundColumnLayoutTest, AdmissionPrecedesUpstreamAndChargesOnlyRetainedAllocatorRequests) {
	MaterializedByteBudget budget(1U);
	LayoutAllocator allocator;
	const std::vector<std::string> names{std::string(256U, 'a'), "b"};
	EXPECT_THROW((BoundColumnLayout(names, budget, &allocator)), MaterializedByteLimitExceeded);
	EXPECT_EQ(allocator.calls, 0U);
	EXPECT_EQ(budget.used(), 0U);
	MaterializedByteBudget ample(65536U);
	{
		BoundColumnLayout layout(names, ample, &allocator);
		EXPECT_EQ(ample.used(), allocator.live);
		EXPECT_GT(ample.used(), names.front().size());
	}
	EXPECT_EQ(ample.used(), 0U);
	EXPECT_EQ(allocator.live, 0U);
}

TEST(BoundColumnLayoutTest, EveryAllocationFailureRefundsBudgetAndAllowsRetry) {
	const std::vector<std::string> names{std::string(256U, 'a'), "b"};
	MaterializedByteBudget budget(65536U);
	LayoutAllocator baseline;
	{ BoundColumnLayout layout(names, budget, &baseline); }
	for (size_t failure = 0U; failure < baseline.calls; ++failure) {
		SCOPED_TRACE(failure);
		LayoutAllocator allocator;
		allocator.fail_at = failure;
		EXPECT_THROW((BoundColumnLayout(names, budget, &allocator)), std::bad_alloc);
		EXPECT_EQ(budget.used(), 0U);
		EXPECT_EQ(allocator.live, 0U);
		allocator.fail_at = (std::numeric_limits<size_t>::max)();
		{ BoundColumnLayout retry(names, budget, &allocator); EXPECT_EQ(retry.size(), 2U); }
		EXPECT_EQ(budget.used(), 0U);
		EXPECT_EQ(allocator.live, 0U);
	}
}

TEST(BoundColumnLayoutTest, EveryGuardFailureRefundsOwnedAndValidationAllocations) {
	const std::vector<std::string> names{std::string(256U, 'a'), "b"};
	MaterializedByteBudget budget(65536U);
	size_t checkpoints = 0U;
	{ BoundColumnLayout layout(names, budget, std::pmr::get_default_resource(), [&] { ++checkpoints; }); }
	for (size_t failure = 1U; failure <= checkpoints; ++failure) {
		size_t current = 0U;
		EXPECT_THROW((BoundColumnLayout(names, budget, std::pmr::get_default_resource(), [&] {
			if (++current == failure) throw std::runtime_error("cancel layout binding");
		})), std::runtime_error);
		EXPECT_EQ(budget.used(), 0U);
	}
}
} // namespace
} // namespace graph::query::execution::detail
