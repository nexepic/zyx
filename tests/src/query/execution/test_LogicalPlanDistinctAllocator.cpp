#include <gtest/gtest.h>
#include <memory_resource>
#include <algorithm>
#include <limits>
#include "CypherParserImpl.hpp"
#include "query/execution/LogicalPlanUnwindRuntimeTestFixture.hpp"

namespace graph::query::execution {
namespace {
class DistinctAllocationObserver final : public std::pmr::memory_resource {
public:
	size_t live = 0U, peak = 0U, allocations = 0U, deallocations = 0U;
	size_t allocation_limit = std::numeric_limits<size_t>::max();
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (allocations == allocation_limit) throw std::bad_alloc();
		void *pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
		live += bytes; peak = std::max(peak, live); ++allocations;
		return pointer;
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
		live -= bytes; ++deallocations;
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
class ScopedDistinctResource final {
public:
	explicit ScopedDistinctResource(std::pmr::memory_resource &resource) : previous_(std::pmr::set_default_resource(&resource)) {}
	~ScopedDistinctResource() { std::pmr::set_default_resource(previous_); }
private:
	std::pmr::memory_resource *previous_;
};
}

TEST(LogicalPlanDistinctAllocatorTest, ProjectAndUnionOwnChargedKeysAndReleaseThemBeforeResultsEscape) {
	const std::string a(256U, 'a'), b(256U, 'b');
	const std::vector<std::string> queries{
		"UNWIND ['" + a + "','" + a + "','" + b + "'] AS value RETURN DISTINCT value",
		"RETURN '" + a + "' AS value UNION RETURN '" + b + "' AS value"};
	size_t queryIndex = 0U;
	for (const auto &query : queries) {
		graph::parser::cypher::CypherParserImpl parser;
		auto plan = parser.parseToLogical(query);
		test::NoGraphBackend backend; LogicalPlanReadRuntime runtime(backend);
		DistinctAllocationObserver observer;
		ScopedDistinctResource resource(observer);
		const auto result = runtime.executeToQueryResult(*plan.root, test::bounds());
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_GT(observer.allocations, 0U); EXPECT_GT(observer.peak, 2U * a.size());
		EXPECT_EQ(observer.live, 0U); EXPECT_EQ(observer.allocations, observer.deallocations);
		const std::string prefix = queryIndex++ == 0U ? "project_" : "union_";
		RecordProperty(prefix + "allocation_requests", std::to_string(observer.allocations));
		RecordProperty(prefix + "peak_requested_bytes", std::to_string(observer.peak));
		RecordProperty(prefix + "retained_after_execution", std::to_string(observer.live));

		EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(a));
		size_t delivered = 0U;
		static_cast<void>(runtime.execute(*plan.root, test::bounds(), [&](const LogicalPlanReadRow &) { ++delivered; return false; }));
		EXPECT_EQ(delivered, 1U); EXPECT_EQ(observer.live, 0U);
		EXPECT_EQ(observer.allocations, observer.deallocations);
	}
}

TEST(LogicalPlanDistinctAllocatorTest, AggregateGroupsAndDistinctKeysReleaseTheirChargedAllocations) {
	const std::string a(256U, 'a'), b(256U, 'b');
	const std::string query = "UNWIND [{g:1,v:'" + a + "'},{g:1,v:'" + a +
		"'},{g:2,v:'" + b + "'},{g:2,v:null}] AS row RETURN row.g AS g, count(DISTINCT row.v) AS n";
	graph::parser::cypher::CypherParserImpl parser;
	auto plan = parser.parseToLogical(query);
	test::NoGraphBackend backend; LogicalPlanReadRuntime runtime(backend);
	DistinctAllocationObserver observer; ScopedDistinctResource resource(observer);
	const auto result = runtime.executeToQueryResult(*plan.root, test::bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	for (const auto &row : result.getRows()) EXPECT_EQ(row.at("n").asPrimitive(), PropertyValue(1));
	EXPECT_GT(observer.allocations, 0U); EXPECT_GT(observer.peak, 2U * a.size());
	EXPECT_EQ(observer.live, 0U); EXPECT_EQ(observer.allocations, observer.deallocations);
	RecordProperty("aggregate_allocation_requests", std::to_string(observer.allocations));
	RecordProperty("aggregate_peak_requested_bytes", std::to_string(observer.peak));
	RecordProperty("aggregate_retained_after_execution", std::to_string(observer.live));
	size_t delivered = 0U;
	static_cast<void>(runtime.execute(*plan.root, test::bounds(), [&](const LogicalPlanReadRow &) { ++delivered; return false; }));
	EXPECT_EQ(delivered, 1U); EXPECT_EQ(observer.live, 0U);
	auto limits = test::bounds(); limits.maximum_materialized_bytes = 1024U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limits)), std::runtime_error);
	EXPECT_EQ(observer.live, 0U); EXPECT_EQ(observer.allocations, observer.deallocations);
	observer.allocation_limit = observer.allocations + 3U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, test::bounds())), std::bad_alloc);
	EXPECT_EQ(observer.live, 0U); EXPECT_EQ(observer.allocations, observer.deallocations);
	observer.allocation_limit = std::numeric_limits<size_t>::max();
	EXPECT_NO_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, test::bounds())));
	EXPECT_EQ(observer.live, 0U); EXPECT_EQ(observer.allocations, observer.deallocations);
}

TEST(LogicalPlanDistinctAllocatorTest, PercentileRuntimeArraysUseChargedStorageAndUnwindAllocationFailure) {
	graph::parser::cypher::CypherParserImpl parser;
	auto plan = parser.parseToLogical("UNWIND [9,1,5,3,7,2] AS v RETURN percentileDisc(v,0.25) AS d, percentileCont(v,0.25) AS c");
	test::NoGraphBackend backend; LogicalPlanReadRuntime runtime(backend);
	DistinctAllocationObserver observer; ScopedDistinctResource resource(observer);
	const auto result = runtime.executeToQueryResult(*plan.root, test::bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("d").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(result.getRows().front().at("c").asPrimitive(), PropertyValue(2.25));
	EXPECT_GT(observer.allocations, 0U); EXPECT_EQ(observer.live, 0U);
	RecordProperty("percentile_array_requests", std::to_string(observer.allocations));
	RecordProperty("percentile_array_peak_requested_bytes", std::to_string(observer.peak));
	observer.allocation_limit = observer.allocations + 3U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, test::bounds())), std::bad_alloc);
	EXPECT_EQ(observer.live, 0U); EXPECT_EQ(observer.allocations, observer.deallocations);
	observer.allocation_limit = std::numeric_limits<size_t>::max();
	EXPECT_NO_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, test::bounds())));
	EXPECT_EQ(observer.live, 0U); EXPECT_EQ(observer.allocations, observer.deallocations);
}

TEST(LogicalPlanDistinctAllocatorTest, NestedGroupPayloadPreservesDuplicateGroupingAndRetryAfterBudgetFailure) {
	const std::string payload(512U, 'x');
	graph::parser::cypher::CypherParserImpl parser;
	auto plan = parser.parseToLogical("UNWIND [1,1,2] AS n RETURN {k:n,s:['" + payload + "']} AS g, count(*) AS c");
	test::NoGraphBackend backend; LogicalPlanReadRuntime runtime(backend);
	DistinctAllocationObserver observer; ScopedDistinctResource resource(observer);
	const auto result = runtime.executeToQueryResult(*plan.root, test::bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	for (const auto &row : result.getRows()) {
		const auto group = row.at("g").asPrimitive();
		const auto key = group.getMap().at("k");
		EXPECT_EQ(group.getMap().at("s").getList().front(), PropertyValue(payload));
		EXPECT_EQ(row.at("c").asPrimitive(), key == PropertyValue(1) ? PropertyValue(2) : PropertyValue(1));
	}
	EXPECT_EQ(observer.live, 0U);
	auto limits = test::bounds(); limits.maximum_materialized_bytes = 1024U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limits)), std::runtime_error);
	EXPECT_EQ(observer.live, 0U);
	EXPECT_NO_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, test::bounds())));
	EXPECT_EQ(observer.live, 0U); EXPECT_EQ(observer.allocations, observer.deallocations);
}

} // namespace graph::query::execution
