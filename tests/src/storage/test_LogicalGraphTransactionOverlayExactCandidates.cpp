/**
 * @brief Transaction overlay exact candidate provenance and fallback regression.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include <gtest/gtest.h>
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "storage/LogicalGraphTransactionOverlayTestFixture.hpp"

namespace graph::storage {
namespace {
using namespace transaction_overlay_test;

IndexCandidateBatch verifiedBatch(std::initializer_list<uint64_t> ids) {
	IndexCandidateBatch batch;
	batch.snapshot_generation = 1U;
	batch.exact_match_verified = true;
	batch.predicates_verified = true;
	batch.label_scope_verified = true;
	for (uint64_t id: ids) batch.candidates.push_back({id, false, 0U, 0U, 0U});
	return batch;
}

TEST(LogicalGraphTransactionOverlayTest, ExactCandidateStreamValidatesBeforeHydrationAndAllowsRetry) {
	InMemoryLogicalTransaction transaction;
	auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
	base.native_exact_candidates = true;
	base.exact_candidate_generation = 1U;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto find = [&](size_t bound = 2U) {
		return snapshot->findNodesByExactProperties({"Base"}, {{"name", PropertyValue("Ada")}}, bound, 1U);
	};
	const auto install = [&](IndexCandidateBatch first, IndexCandidateBatch second, bool complete = true) {
		base.exact_candidate_stream = [first = std::move(first), second = std::move(second), complete](const auto &visitor) {
			if (!visitor(first) || !visitor(second)) return std::optional<bool>{false};
			return std::optional<bool>{complete};
		};
	};
	for (size_t defect = 0U; defect < 7U; ++defect) {
		SCOPED_TRACE(defect);
		auto first = verifiedBatch({1U});
		auto second = verifiedBatch({});
		if (defect == 0U) second.snapshot_generation = 2U;
		if (defect == 1U) second.snapshot_generation = 0U;
		if (defect == 2U) second = verifiedBatch({1U});
		if (defect == 3U) second = verifiedBatch({0U});
		if (defect == 4U) { second = verifiedBatch({2U}); second.candidates.front().is_edge = true; }
		if (defect == 5U) second = verifiedBatch({2U, 3U});
		install(std::move(first), std::move(second), defect != 6U);
		base.read_node_calls = 0U;
		EXPECT_THROW(static_cast<void>(find()), std::runtime_error);
		EXPECT_EQ(base.read_node_calls, 0U);
		install(verifiedBatch({1U}), verifiedBatch({}));
		const auto result = find();
		ASSERT_TRUE(result); ASSERT_EQ(result->size(), 1U);
		EXPECT_EQ(result->front().node_id, 1U);
		EXPECT_EQ(base.read_node_calls, 1U);
	}
}

TEST(LogicalGraphTransactionOverlayTest, ExactCandidateStreamFallsBackForUnsupportedOrUnverifiedProof) {
	InMemoryLogicalTransaction transaction;
	auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
	base.native_exact_candidates = true;
	base.exact_candidate_generation = 1U;
	size_t fallbackCalls = 0U;
	base.exact_node_lookup = [&](const auto &, const auto &, size_t, size_t) {
		++fallbackCalls;
		return std::optional<std::vector<LogicalGraphNode>>{{base.nodes.at(1U)}};
	};
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	for (size_t missingProof = 0U; missingProof < 4U; ++missingProof) {
		SCOPED_TRACE(missingProof);
		auto batch = verifiedBatch({1U});
		if (missingProof == 0U) batch.exact_match_verified = false;
		if (missingProof == 1U) batch.predicates_verified = false;
		if (missingProof == 2U) batch.label_scope_verified = false;
		base.exact_candidate_stream = [batch, missingProof](const auto &visitor) -> std::optional<bool> {
			if (missingProof == 3U) return std::nullopt;
			return visitor(batch);
		};
		const auto result = snapshot->findNodesByExactProperties({"Base"}, {{"name", PropertyValue("Ada")}}, 2U, 1U);
		ASSERT_TRUE(result); ASSERT_EQ(result->size(), 1U);
		EXPECT_EQ(result->front().node_id, 1U);
		EXPECT_EQ(fallbackCalls, missingProof + 1U);
	}
}

TEST(LogicalGraphTransactionOverlayTest, NativeExactStreamRejectsBrokenColumnsAndStopsSourceBeforeTail) {
	InMemoryLogicalTransaction transaction;
	auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
	base.native_exact_candidates = true;
	base.exact_candidate_generation = 1U;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto read = [&](const IndexCandidateBatchVisitor &visitor) {
		return snapshot->visitNodeCandidatesByExactProperties({"Base"}, {{"name", PropertyValue("Ada")}}, 2U, 1U, 4096U, visitor);
	};
	for (size_t defect = 0U; defect < 4U; ++defect) {
		SCOPED_TRACE(defect);
		auto batch = verifiedBatch({1U});
		batch.verified_property_columns.push_back({"name", {PropertyValue("Ada")}});
		if (defect == 0U) batch.verified_property_columns.clear();
		if (defect == 1U) batch.verified_property_columns.front().values.clear();
		if (defect == 2U) batch.verified_property_columns.push_back(batch.verified_property_columns.front());
		if (defect == 3U) batch.snapshot_generation = 2U;
		base.exact_candidate_stream = [batch](const auto &visitor) { return std::optional<bool>{visitor(batch)}; };
		size_t deliveries = 0U;
		EXPECT_THROW(static_cast<void>(read([&](const auto &) { ++deliveries; return true; })), std::runtime_error);
		EXPECT_EQ(deliveries, 0U);
		EXPECT_EQ(base.read_node_calls, 0U);
	}
	auto batch = verifiedBatch({1U});
	batch.verified_property_columns.push_back({"name", {PropertyValue("Ada")}});
	size_t sourceTail = 0U;
	base.exact_candidate_stream = [&](const auto &visitor) {
		if (!visitor(batch)) return std::optional<bool>{false};
		++sourceTail;
		return std::optional<bool>{true};
	};
	EXPECT_EQ(read([](const auto &) { return false; }), std::optional<bool>{false});
	EXPECT_EQ(sourceTail, 0U);
	EXPECT_EQ(read([](const auto &) { return true; }), std::optional<bool>{true});
	EXPECT_EQ(sourceTail, 1U);
	EXPECT_EQ(base.read_node_calls, 0U);
	base.exact_candidate_stream = [](const auto &visitor) { return std::optional<bool>{visitor(verifiedBatch({}))}; };
	size_t emptyDeliveries = 0U;
	EXPECT_EQ(read([&](const auto &) { ++emptyDeliveries; return true; }), std::optional<bool>{true});
	EXPECT_EQ(emptyDeliveries, 0U);
	const auto empty = snapshot->findNodeCandidatesByExactProperties({"Base"}, {{"name", PropertyValue("Ada")}}, 2U, 1U);
	ASSERT_TRUE(empty);
	EXPECT_TRUE(empty->candidates.empty());
	EXPECT_TRUE(empty->exact_match_verified && empty->predicates_verified && empty->label_scope_verified);
}
}
}
