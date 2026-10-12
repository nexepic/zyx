#include <gtest/gtest.h>

#include "query/execution/BoundProjectionReferences.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
namespace {

class FailingBindingResource final : public std::pmr::memory_resource {
public:
	size_t requests = 0U;
	size_t fail_at = (std::numeric_limits<size_t>::max)();
	size_t live_bytes = 0U;
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (requests++ == fail_at) throw std::bad_alloc();
		void *pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
		live_bytes += bytes;
		return pointer;
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
		live_bytes -= bytes;
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

logical::LogicalProjectItem reference(std::string variable, std::string alias) {
	return {std::make_shared<expressions::VariableReferenceExpression>(std::move(variable)), std::move(alias)};
}
logical::LogicalProjectItem property(std::string variable, std::string key, std::string alias) {
	return {std::make_shared<expressions::VariableReferenceExpression>(std::move(variable), std::move(key)), std::move(alias)};
}

TEST(BoundProjectionReferencesTest, SharedVariableReferencesConsumeTheSameSlot) {
	MaterializedByteBudget budget(65536U);
	const std::vector<std::string> inputs{"n", "n", "unused"};
	const std::vector<logical::LogicalProjectItem> items{property("n", "a", "a"), property("n", "b", "b"), reference("n", "entity")};
	detail::BoundProjectionReferences program(inputs, items, budget);
	EXPECT_EQ(program.slotCount(), 2U);
	EXPECT_EQ(program.instruction(0).source, program.instruction(1).source);
	EXPECT_EQ(program.instruction(0).source, program.instruction(2).source);
	EXPECT_EQ(program.instruction(0).kind, detail::ProjectionReferenceKind::PRK_PROPERTY);
	EXPECT_EQ(program.instruction(2).kind, detail::ProjectionReferenceKind::PRK_VALUE);
	LogicalPlanReadRow row;
	row.values.emplace("n", PropertyValue(int64_t{42}));
	row.values.emplace("unknown", PropertyValue(int64_t{9}));
	detail::BoundProjectionReferences::RowBinding binding(program, row);
	EXPECT_EQ(program.value(program.instruction(0)), &row.values.at("n"));
	EXPECT_EQ(program.value(program.instruction(2)), &row.values.at("n"));
}

TEST(BoundProjectionReferencesTest, ProjectedPropertyColumnWinsOverAnEntityWithTheSameVariable) {
	MaterializedByteBudget budget(65536U);
	const std::vector<std::string> inputs{"n", "n.value"};
	const std::vector<logical::LogicalProjectItem> items{property("n", "value", "v")};
	detail::BoundProjectionReferences program(inputs, items, budget);
	LogicalPlanReadRow row;
	row.values.emplace("n", PropertyValue(int64_t{1}));
	row.values.emplace("n.value", PropertyValue(int64_t{7}));
	detail::BoundProjectionReferences::RowBinding binding(program, row);
	EXPECT_EQ(program.instruction(0).kind, detail::ProjectionReferenceKind::PRK_VALUE);
	EXPECT_EQ(program.value(program.instruction(0)), &row.values.at("n.value"));
}

TEST(BoundProjectionReferencesTest, DynamicExpressionsAndUnknownReferencesRetainSemanticFallback) {
	MaterializedByteBudget budget(65536U);
	const std::vector<std::string> inputs{"n"};
	const std::vector<logical::LogicalProjectItem> items{reference("unknown", "x"),
		{std::make_shared<expressions::LiteralExpression>(int64_t{3}), "constant"}};
	detail::BoundProjectionReferences program(inputs, items, budget);
	LogicalPlanReadRow row;
	detail::BoundProjectionReferences::RowBinding binding(program, row);
	EXPECT_EQ(program.instruction(0).kind, detail::ProjectionReferenceKind::PRK_DYNAMIC);
	EXPECT_EQ(program.instruction(1).kind, detail::ProjectionReferenceKind::PRK_DYNAMIC);
	EXPECT_EQ(program.value(program.instruction(0)), nullptr);
	EXPECT_EQ(program.value(program.instruction(1)), nullptr);
	EXPECT_THROW(static_cast<void>(program.instruction(2)), std::out_of_range);
}

TEST(BoundProjectionReferencesTest, MissingInputAndPresentNullAreDistinctAndRowsDoNotRetainPointers) {
	MaterializedByteBudget budget(65536U);
	const std::vector<std::string> inputs{"n"};
	const std::vector<logical::LogicalProjectItem> items{reference("n", "n")};
	detail::BoundProjectionReferences program(inputs, items, budget);
	LogicalPlanReadRow nullRow;
	nullRow.values.emplace("n", PropertyValue{});
	{
		detail::BoundProjectionReferences::RowBinding binding(program, nullRow);
		ASSERT_NE(program.value(program.instruction(0)), nullptr);
		EXPECT_EQ(std::get<PropertyValue>(*program.value(program.instruction(0))).getType(), PropertyType::NULL_TYPE);
	}
	nullRow.values.clear();
	EXPECT_THROW(static_cast<void>(program.value(program.instruction(0))), std::logic_error);
	{
		detail::BoundProjectionReferences::RowBinding binding(program, nullRow);
		EXPECT_EQ(program.value(program.instruction(0)), nullptr);
	}
}

TEST(BoundProjectionReferencesTest, NestedBindingAndForeignInstructionsAreRejectedWithoutLosingTheCurrentRow) {
	MaterializedByteBudget budget(65536U);
	const std::vector<std::string> inputs{"n"};
	const std::vector<logical::LogicalProjectItem> items{reference("n", "n")};
	detail::BoundProjectionReferences first(inputs, items, budget), second(inputs, items, budget);
	LogicalPlanReadRow row;
	row.values.emplace("n", PropertyValue(int64_t{1}));
	detail::BoundProjectionReferences::RowBinding binding(first, row);
	EXPECT_THROW((detail::BoundProjectionReferences::RowBinding(first, row)), std::logic_error);
	EXPECT_THROW(static_cast<void>(first.value(second.instruction(0))), std::invalid_argument);
	EXPECT_EQ(first.value(first.instruction(0)), &row.values.at("n"));
}

TEST(BoundProjectionReferencesTest, ExceptionsReleaseRowBorrowsAndOwnedAllocations) {
	MaterializedByteBudget budget(65536U);
	const std::vector<std::string> inputs{std::string(256U, 'n')};
	const std::vector<logical::LogicalProjectItem> items{reference(inputs.front(), "n")};
	{
		detail::BoundProjectionReferences program(inputs, items, budget);
		EXPECT_GT(budget.used(), 0U);
		LogicalPlanReadRow row;
		row.values.emplace(inputs.front(), PropertyValue(int64_t{1}));
		try {
			detail::BoundProjectionReferences::RowBinding binding(program, row);
			throw std::runtime_error("consumer failed");
		} catch (const std::runtime_error &) {}
		EXPECT_THROW(static_cast<void>(program.value(program.instruction(0))), std::logic_error);
	}
	EXPECT_EQ(budget.used(), 0U);
	MaterializedByteBudget tiny(1U);
	EXPECT_THROW((detail::BoundProjectionReferences(inputs, items, tiny)), std::runtime_error);
	EXPECT_EQ(tiny.used(), 0U);
}

TEST(BoundProjectionReferencesTest, EveryConstructionAllocationFailureRefundsBudgetAndAllowsRetry) {
	const std::vector<std::string> inputs{std::string(256U, 'n'), "other"};
	const std::vector<logical::LogicalProjectItem> items{reference(inputs.front(), "first"), property("other", "v", "v")};
	FailingBindingResource baseline;
	MaterializedByteBudget baselineBudget(65536U);
	{
		detail::BoundProjectionReferences program(inputs, items, baselineBudget, &baseline);
	}
	ASSERT_GT(baseline.requests, 0U);
	for (size_t failure = 0U; failure < baseline.requests; ++failure) {
		SCOPED_TRACE(failure);
		FailingBindingResource resource;
		resource.fail_at = failure;
		MaterializedByteBudget budget(65536U);
		EXPECT_THROW((detail::BoundProjectionReferences(inputs, items, budget, &resource)), std::bad_alloc);
		EXPECT_EQ(resource.live_bytes, 0U);
		EXPECT_EQ(budget.used(), 0U);
		resource.fail_at = (std::numeric_limits<size_t>::max)();
		{
			detail::BoundProjectionReferences program(inputs, items, budget, &resource);
			EXPECT_GT(resource.live_bytes, 0U);
		}
		EXPECT_EQ(resource.live_bytes, 0U);
		EXPECT_EQ(budget.used(), 0U);
	}
	EXPECT_THROW((detail::BoundProjectionReferences(inputs, items, baselineBudget, nullptr)), std::invalid_argument);
	EXPECT_EQ(baselineBudget.used(), 0U);
}

TEST(BoundProjectionReferencesTest, RuntimeProjectionPreservesProjectedPropertyPriorityAndDistinctRows) {
	using namespace direct_edge_test;
	for (bool distinct : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalProjectItem> inputItems{reference("n", "n"),
			{std::make_shared<expressions::LiteralExpression>(int64_t{77}), "n.rank"}};
		auto input = std::make_unique<logical::LogicalProject>(
			std::make_unique<logical::LogicalNodeScan>("n"), std::move(inputItems));
		std::vector<logical::LogicalProjectItem> outputItems{property("n", "rank", "rank"), property("n", "rank", "again")};
		logical::LogicalProject project(std::move(input), std::move(outputItems), distinct);
		struct TraceScope {
			bool previous = debug::PerfTrace::areValuesEnabled();
			TraceScope() { debug::PerfTrace::reset(); debug::PerfTrace::setValueCollectionEnabled(true); }
			~TraceScope() { debug::PerfTrace::setValueCollectionEnabled(previous); }
		} traceScope;
		size_t delivered = 0U;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const auto &row) {
			++delivered;
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("rank")), PropertyValue(int64_t{77}));
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("again")), PropertyValue(int64_t{77}));
			return true;
		}));
		EXPECT_EQ(delivered, distinct ? 1U : graph.nodes.size());
		const auto trace = debug::PerfTrace::snapshotAndReset();
		ASSERT_TRUE(trace.contains("logical_plan.bound_projection.slot_reads"));
		// Each input row reads n in the inner projection and the two projected
		// property columns in the outer projection, even before DISTINCT collapses rows.
		EXPECT_EQ(trace.at("logical_plan.bound_projection.slot_reads").totalValue,
			static_cast<int64_t>(3U * graph.nodes.size()));
	}
}

TEST(BoundProjectionReferencesTest, RuntimeMixedProjectionPreservesOwnedOutputEarlyStopAndRetryAfterErrors) {
	using namespace direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	std::vector<logical::LogicalProjectItem> items{property("n", "rank", "rank"), property("n", "rank", "again"),
		{std::make_shared<expressions::LiteralExpression>(std::string(512U, 'x')), "payload"}};
	logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), std::move(items));
	EXPECT_THROW(static_cast<void>(runtime.execute(project, directBounds(), [](const auto &) -> bool {
		throw std::runtime_error("consumer failed");
	})), std::runtime_error);
	std::vector<LogicalPlanReadRow> escaped;
	static_cast<void>(runtime.execute(project, directBounds(), [&](const auto &row) {
		escaped.push_back(row);
		return false;
	}));
	ASSERT_EQ(escaped.size(), 1U);
	graph.nodes.clear();
	EXPECT_EQ(escaped.front().values.at("rank"), escaped.front().values.at("again"));
	EXPECT_EQ(std::get<PropertyValue>(escaped.front().values.at("payload")), PropertyValue(std::string(512U, 'x')));
}

TEST(BoundProjectionReferencesTest, RuntimeLocalComprehensionShadowingRestoresBoundEntitySlots) {
	using namespace direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto comprehension = std::make_shared<expressions::ListComprehensionExpression>("n",
		std::make_unique<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{PropertyValue(10), PropertyValue(20)})),
		nullptr, std::make_unique<expressions::VariableReferenceExpression>("n"),
		expressions::ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
	std::vector<logical::LogicalProjectItem> items{property("n", "rank", "before"),
		{comprehension, "local"}, property("n", "rank", "after")};
	logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), std::move(items));
	size_t delivered = 0U;
	static_cast<void>(runtime.execute(project, directBounds(), [&](const auto &row) {
		++delivered;
		EXPECT_EQ(row.values.at("before"), row.values.at("after"));
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("local")),
			PropertyValue(std::vector<PropertyValue>{PropertyValue(10), PropertyValue(20)}));
		return true;
	}));
	EXPECT_EQ(delivered, graph.nodes.size());
}

} // namespace
} // namespace graph::query::execution
