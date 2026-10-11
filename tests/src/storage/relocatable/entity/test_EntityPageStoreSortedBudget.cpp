/** @file test_EntityPageStoreSortedBudget.cpp
 *  @brief Sorted Edge batch admission and sparse directory results.
 */
#include "storage/relocatable/entity/EntityPageStoreTestFixture.hpp"
#include <algorithm>

TEST_F(RelocatableEntityPageStoreTest, SortedEdgeBuffersAdmitBeforeAllocationAndReleaseLocationStaging) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const EdgeEntityRecord first = edge(10U, 11U, 21U);
	const EdgeEntityRecord second = edge(12U, 13U, 22U);
	ASSERT_TRUE(store.allocateEdge(newPool(31U, 901U, 201U, 40U), 2001U, first, LogSequenceNumber(2U)).has_value());
	ASSERT_TRUE(store.allocateEdge(newPool(32U, 901U, 202U, 41U), 2003U, second, LogSequenceNumber(3U)).has_value());
	const std::array<uint64_t, 3U> ids{2001U, 2002U, 2003U};
	auto snapshot = metadata->openSnapshot();
	size_t observedBytes = 0U;
	EXPECT_THROW(static_cast<void>(store.readEdgesSorted(snapshot, ids, [&](size_t bytes) {
		observedBytes = bytes;
		throw std::runtime_error("result allocation rejected");
	})), std::runtime_error);
	EXPECT_EQ(observedBytes, ids.size() * sizeof(std::optional<EdgeEntityRecord>));
	const size_t resultAllowance = observedBytes;
	EXPECT_THROW(static_cast<void>(store.readEdgesSorted(snapshot, ids, [&](size_t bytes) {
		observedBytes = bytes;
		if (bytes > resultAllowance) throw std::runtime_error("location allocation rejected");
	})), std::runtime_error);
	EXPECT_GT(observedBytes, resultAllowance);
	size_t peakBytes = 0U;
	const auto result = store.readEdgesSorted(snapshot, ids, [&](size_t bytes) {
		observedBytes = bytes;
		peakBytes = std::max(peakBytes, bytes);
	});
	ASSERT_EQ(result.size(), 3U);
	EXPECT_EQ(result[0], first);
	EXPECT_FALSE(result[1].has_value());
	EXPECT_EQ(result[2], second);
	EXPECT_EQ(observedBytes, result.capacity() * sizeof(std::optional<EdgeEntityRecord>));
	EXPECT_GE(peakBytes, ids.size() * (sizeof(std::optional<EdgeEntityRecord>) + sizeof(std::optional<EntityPageLocation>)) +
		2U * sizeof(size_t));
	// Validation rejects malformed input before any result reservation.
	size_t calls = 0U;
	expectFormatError([&] {
		static_cast<void>(store.readEdgesSorted(snapshot, std::array<uint64_t, 2U>{2003U, 2001U},
			[&](size_t) { ++calls; }));
	}, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(calls, 0U);
}

TEST_F(RelocatableEntityPageStoreTest, SortedNodeBuffersAdmitBeforeAllocationAndReleaseLocationStaging) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord first = node(0U, 0U, 21U);
	const NodeEntityRecord second = node(0U, 0U, 22U);
	ASSERT_TRUE(store.allocateNode(newPool(31U, 901U, 201U, 40U), 2001U, first, LogSequenceNumber(2U)).has_value());
	ASSERT_TRUE(store.allocateNode(newPool(32U, 901U, 202U, 41U), 2003U, second, LogSequenceNumber(3U)).has_value());
	const std::array<uint64_t, 3U> ids{2001U, 2002U, 2003U};
	auto snapshot = metadata->openSnapshot();
	size_t observedBytes = 0U;
	EXPECT_THROW(static_cast<void>(store.readNodesSorted(snapshot, ids, [&](size_t bytes) {
		observedBytes = bytes;
		throw std::runtime_error("result allocation rejected");
	})), std::runtime_error);
	EXPECT_EQ(observedBytes, ids.size() * sizeof(std::optional<NodeEntityRecord>));
	const size_t resultAllowance = observedBytes;
	EXPECT_THROW(static_cast<void>(store.readNodesSorted(snapshot, ids, [&](size_t bytes) {
		observedBytes = bytes;
		if (bytes > resultAllowance) throw std::runtime_error("location allocation rejected");
	})), std::runtime_error);
	EXPECT_GT(observedBytes, resultAllowance);
	size_t peakBytes = 0U;
	const auto result = store.readNodesSorted(snapshot, ids, [&](size_t bytes) {
		observedBytes = bytes;
		peakBytes = std::max(peakBytes, bytes);
	});
	ASSERT_EQ(result.size(), 3U);
	EXPECT_EQ(result[0], first);
	EXPECT_FALSE(result[1].has_value());
	EXPECT_EQ(result[2], second);
	EXPECT_EQ(observedBytes, result.capacity() * sizeof(std::optional<NodeEntityRecord>));
	EXPECT_GE(peakBytes, ids.size() * (sizeof(std::optional<NodeEntityRecord>) + sizeof(std::optional<EntityPageLocation>)) +
		2U * sizeof(size_t));
	// Validation rejects malformed input before any result reservation.
	size_t calls = 0U;
	expectFormatError([&] {
		static_cast<void>(store.readNodesSorted(snapshot, std::array<uint64_t, 2U>{2003U, 2001U},
			[&](size_t) { ++calls; }));
	}, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(calls, 0U);
}
