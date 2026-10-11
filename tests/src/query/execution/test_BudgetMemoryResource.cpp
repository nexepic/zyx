#include <gtest/gtest.h>
#include <memory_resource>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <vector>
#include "query/execution/BudgetMemoryResource.hpp"
using namespace graph::query::execution;

namespace {
class CountingResource final : public std::pmr::memory_resource {
public:
	size_t allocations = 0U;
	size_t deallocations = 0U;
	size_t live_bytes = 0U;
	size_t peak_bytes = 0U;
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		++allocations;
		void *pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
		live_bytes += bytes; peak_bytes = std::max(peak_bytes, live_bytes);
		return pointer;
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		++deallocations;
		live_bytes -= bytes;
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}

TEST(BudgetMemoryResourceTest, ChargesLiveAllocationsAndRejectsBeforeUpstream) {
	MaterializedByteBudget budget(128U);
	CountingResource upstream;
	detail::BudgetMemoryResource resource(budget, &upstream);
	void *first = resource.allocate(80U, alignof(std::max_align_t));
	EXPECT_EQ(budget.used(), 80U);
	EXPECT_THROW(static_cast<void>(resource.allocate(49U, alignof(std::max_align_t))), std::runtime_error);
	EXPECT_EQ(budget.used(), 80U);
	EXPECT_EQ(upstream.allocations, 1U);
	resource.deallocate(first, 80U, alignof(std::max_align_t));
	EXPECT_EQ(upstream.deallocations, 1U);
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BudgetMemoryResourceTest, UpstreamFailureRollsBackAndContainersReleaseBucketsAndNodes) {
	MaterializedByteBudget budget(8192U);
	detail::BudgetMemoryResource rejecting(budget, std::pmr::null_memory_resource());
	EXPECT_THROW(static_cast<void>(rejecting.allocate(16U)), std::bad_alloc);
	EXPECT_EQ(budget.used(), 0U);
	detail::BudgetMemoryResource resource(budget);
	{
		std::pmr::unordered_map<uint64_t, uint64_t> values(&resource);
		values.reserve(16U);
		const auto bucketBytes = budget.used();
		EXPECT_GT(bucketBytes, 0U);
		values.emplace(1U, 2U);
		EXPECT_GT(budget.used(), bucketBytes);
		values.erase(1U);
		EXPECT_EQ(budget.used(), bucketBytes);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BudgetMemoryResourceTest, VectorGrowthChargesOverlapAndKeepsOldElementsAfterRejection) {
	MaterializedByteBudget budget(8U * sizeof(uint64_t));
	detail::BudgetMemoryResource resource(budget);
	{
		std::pmr::vector<uint64_t> values(&resource);
		values.reserve(8U);
		values.assign(8U, 42U);
		const auto retained = budget.used();
		EXPECT_THROW(values.push_back(99U), std::runtime_error);
		EXPECT_EQ(values.size(), 8U);
		EXPECT_EQ(values.front(), 42U);
		EXPECT_EQ(budget.used(), retained);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BudgetMemoryResourceTest, HashRehashRejectsOverlapEvenWhenFinalBucketsFit) {
	size_t finalBytes = 0U;
	{
		MaterializedByteBudget probeBudget(1024U * 1024U);
		detail::BudgetMemoryResource probeResource(probeBudget);
		std::pmr::unordered_map<uint64_t, uint64_t> probe(&probeResource);
		probe.reserve(8U);
		probe.emplace(1U, 42U);
		probe.rehash(64U);
		finalBytes = probeBudget.used();
	}
	MaterializedByteBudget budget(finalBytes);
	CountingResource upstream;
	detail::BudgetMemoryResource resource(budget, &upstream);
	{
		std::pmr::unordered_map<uint64_t, uint64_t> values(&resource);
		values.reserve(8U);
		values.emplace(1U, 42U);
		const size_t retained = budget.used();
		const size_t allocations = upstream.allocations;
		EXPECT_THROW(values.rehash(64U), std::runtime_error);
		EXPECT_EQ(upstream.allocations, allocations);
		EXPECT_EQ(budget.used(), retained);
		EXPECT_EQ(values.size(), 1U);
		EXPECT_EQ(values.at(1U), 42U);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BudgetMemoryResourceTest, NestedHashMatchListsInheritBudgetAndPreserveEntriesOnRejection) {
	MaterializedByteBudget budget(1024U);
	CountingResource upstream;
	detail::BudgetMemoryResource resource(budget, &upstream);
	{
		std::pmr::unordered_map<std::string, std::pmr::vector<size_t>> index(&resource);
		auto &matches = index["binding"];
		ASSERT_EQ(matches.get_allocator().resource(), &resource);
		const size_t metadataBytes = budget.used();
		matches.reserve(8U);
		matches.push_back(3U);
		matches.push_back(3U);
		EXPECT_EQ(budget.used(), metadataBytes + matches.capacity() * sizeof(size_t));
		const size_t retained = budget.used();
		const size_t allocations = upstream.allocations;
		EXPECT_THROW(matches.reserve(1024U), std::runtime_error);
		EXPECT_EQ(upstream.allocations, allocations);
		EXPECT_EQ(budget.used(), retained);
		EXPECT_EQ(matches, (std::pmr::vector<size_t>{3U, 3U}));
	}
	EXPECT_EQ(budget.used(), 0U);
	EXPECT_EQ(upstream.allocations, upstream.deallocations);
}

TEST(BudgetMemoryResourceTest, HashOwningKeysAndTransientEncodingShareTheBudget) {
	MaterializedByteBudget budget(4096U);
	detail::BudgetMemoryResource resource(budget);
	const std::string literal(256U, 'x');
	{
		std::pmr::unordered_map<std::pmr::string, std::pmr::vector<size_t>> index(&resource);
		std::pmr::string key{literal, &resource};
		const size_t transientBytes = budget.used();
		EXPECT_EQ(transientBytes, key.capacity() + 1U);
		index[key].push_back(7U);
		ASSERT_EQ(index.begin()->first.get_allocator().resource(), &resource);
		EXPECT_GT(budget.used(), transientBytes + index.begin()->first.capacity());
		const size_t retained = budget.used();
		EXPECT_THROW(key.append(4096U, 'y'), std::runtime_error);
		EXPECT_EQ(std::string_view(key), std::string_view(literal));
		EXPECT_EQ(budget.used(), retained);
		EXPECT_EQ(index.at(key).front(), 7U);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BudgetMemoryResourceTest, DistinctSetChargesOwningKeysRehashOverlapAndRollsBackRejection) {
	MaterializedByteBudget budget(1024U * 1024U);
	CountingResource upstream;
	detail::BudgetMemoryResource resource(budget, &upstream);
	{
		std::pmr::unordered_set<std::pmr::string> keys{&resource};
		keys.emplace(std::pmr::string(256U, 'a', &resource));
		keys.emplace(std::pmr::string(256U, 'b', &resource));
		ASSERT_EQ(keys.size(), 2U);
		for (const auto &key : keys) EXPECT_EQ(key.get_allocator().resource(), &resource);
		EXPECT_EQ(budget.used(), upstream.live_bytes);
		const size_t retained = budget.used(), buckets = keys.bucket_count(), allocations = upstream.allocations;
		MaterializedByteReservation otherConsumer(&budget); otherConsumer.set(budget.available());
		EXPECT_THROW(keys.rehash(buckets * 4U), std::runtime_error);
		EXPECT_EQ(upstream.allocations, allocations); EXPECT_EQ(keys.bucket_count(), buckets);
		EXPECT_EQ(keys.size(), 2U); EXPECT_EQ(upstream.live_bytes, retained);
		otherConsumer.reset(); EXPECT_EQ(budget.used(), retained);
		upstream.peak_bytes = upstream.live_bytes;
		keys.rehash(buckets * 4U);
		EXPECT_GT(upstream.peak_bytes, upstream.live_bytes);
		EXPECT_EQ(budget.used(), upstream.live_bytes);
	}
	EXPECT_EQ(budget.used(), 0U); EXPECT_EQ(upstream.live_bytes, 0U);
	EXPECT_EQ(upstream.allocations, upstream.deallocations);
}
