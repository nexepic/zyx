/**
 * @file test_RelocatableStorageBackendVectorMetrics.cpp
 * @brief End-to-end metric semantics and cosine-domain validation tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"

using namespace graph::storage;

namespace {

	class RelocatableStorageBackendVectorMetricTest : public ::testing::Test {
	protected:
		std::filesystem::path data_path_;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			data_path_ = std::filesystem::temp_directory_path() /
						 (std::string("relocatable_backend_vector_metric_") + to_string(uuid));
		}

		void TearDown() override {
			std::error_code error;
			std::filesystem::remove(data_path_, error);
		}

		[[nodiscard]] RelocatableStorageBackendOptions options() const {
			RelocatableStorageBackendOptions result;
			result.data_path = data_path_;
			result.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
			result.maximum_index_definitions = 8U;
			result.maximum_online_index_entries = 16U;
			result.maximum_online_index_scan_nodes = 16U;
			result.maximum_online_index_pages = 16U;
			result.online_index_sort_run_bytes = 4096U;
			result.maximum_online_index_sort_runs = 4U;
			return result;
		}

		[[nodiscard]] static graph::PropertyValue vectorValue(double first, double second) {
			return graph::PropertyValue(
					std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
		}

		[[nodiscard]] std::shared_ptr<RelocatableStorageBackend> openBackend() const {
			auto backend = RelocatableStorageBackend::initializeNew(options());
			backend->open();
			return backend;
		}

		[[nodiscard]] static LogicalGraphVectorSearchRequest exactRequest(std::string name) {
			LogicalGraphVectorSearchRequest request;
			request.index_name = std::move(name);
			request.query_vector = {1.0F, 0.0F};
			request.top_k = 3U;
			request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
			request.maximum_mapping_entries = 16U;
			request.maximum_raw_vector_reads = 16U;
			return request;
		}
	};

} // namespace

TEST_F(RelocatableStorageBackendVectorMetricTest, RanksExactCosineByAngleRatherThanMagnitude) {
	auto backend = openBackend();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}},
			 {{"Embedding"}, {{"value", vectorValue(2.0, 0.0)}}},
			 {{"Embedding"}, {{"value", vectorValue(10.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend->createLogicalNodeVectorIndex({"embedding_cosine", "Embedding", "value", 2U, "cosine"}));

	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(exactRequest("embedding_cosine"));
	ASSERT_EQ(result.hits.size(), nodes.size());
	EXPECT_EQ(result.hits[0].node_id, nodes[0]);
	EXPECT_EQ(result.hits[1].node_id, nodes[1]);
	EXPECT_EQ(result.hits[2].node_id, nodes[2]);
	EXPECT_FLOAT_EQ(result.hits[0].score, 0.0F);
	EXPECT_FLOAT_EQ(result.hits[1].score, 0.0F);
	EXPECT_GT(result.hits[2].score, 0.0F);

	LogicalGraphVectorSearchRequest zeroQuery = exactRequest("embedding_cosine");
	zeroQuery.query_vector = {0.0F, 0.0F};
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(zeroQuery)), std::invalid_argument);
	snapshot.reset();

	const LogicalGraphVectorTrainingResult trained = backend->trainLogicalVector({"embedding_cosine", 16U, 16U});
	ASSERT_TRUE(trained.trained);
	auto approximateSnapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(approximateSnapshot, nullptr);
	LogicalGraphVectorSearchRequest approximate = exactRequest("embedding_cosine");
	approximate.top_k = 1U;
	approximate.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	approximate.candidate_search_budget = 3U;
	approximate.candidate_refinement_budget = 3U;
	const LogicalGraphVectorSearchResult approximateResult = approximateSnapshot->searchLogicalVector(approximate);
	ASSERT_EQ(approximateResult.hits.size(), 1U);
	EXPECT_TRUE(approximateResult.stats.used_pq);
	EXPECT_EQ(approximateResult.hits.front().node_id, nodes.front());
	approximateSnapshot.reset();
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, RanksTrainedInnerProductByDotProduct) {
	auto backend = openBackend();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}},
			 {{"Embedding"}, {{"value", vectorValue(2.0, 0.0)}}},
			 {{"Embedding"}, {{"value", vectorValue(10.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend->createLogicalNodeVectorIndex({"embedding_ip", "Embedding", "value", 2U, "ip"}));
	ASSERT_TRUE(backend->trainLogicalVector({"embedding_ip", 16U, 16U}).trained);

	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request = exactRequest("embedding_ip");
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.candidate_search_budget = 3U;
	request.candidate_refinement_budget = 3U;
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), 1U);
	EXPECT_TRUE(result.stats.used_pq);
	EXPECT_EQ(result.hits.front().node_id, nodes.back());
	snapshot.reset();
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, KeepsCosinePqLookupIntermediateNormsInDoublePrecision) {
	auto backend = openBackend();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0e30, 0.0)}}},
			 {{"Embedding"}, {{"value", vectorValue(1.0e30, 1.0e24)}}}}));
	static_cast<void>(backend->createLogicalNodeVectorIndex({"embedding_cosine", "Embedding", "value", 2U, "cosine"}));
	ASSERT_TRUE(backend->trainLogicalVector({"embedding_cosine", 16U, 16U}).trained);

	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request = exactRequest("embedding_cosine");
	request.query_vector = {1.0e30F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), 1U);
	EXPECT_TRUE(result.stats.used_pq);
	EXPECT_GE(result.hits.front().score, 0.0F);
	EXPECT_LE(result.hits.front().score, 2.0F);
	snapshot.reset();
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, RejectsZeroNormCosineValuesDuringDdlAndOnlineWrites) {
	{
		auto backend = openBackend();
		static_cast<void>(backend->createLogicalNodes(
				{{{"Embedding"}, {{"value", vectorValue(0.0, 0.0)}}}}));
		EXPECT_THROW(static_cast<void>(backend->createLogicalNodeVectorIndex(
						 {"embedding_cosine", "Embedding", "value", 2U, "cosine"})),
					 std::invalid_argument);
		backend->close();
	}

	std::error_code error;
	std::filesystem::remove(data_path_, error);
	auto backend = openBackend();
	static_cast<void>(backend->createLogicalNodeVectorIndex({"embedding_cosine", "Embedding", "value", 2U, "cosine"}));
	EXPECT_THROW(static_cast<void>(backend->createLogicalNodes(
				 {{{"Embedding"}, {{"value", vectorValue(0.0, 0.0)}}}})),
			 std::invalid_argument);
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, RejectsInvalidQueryShapeValuesAndDerivedSearchBudgets) {
	auto backend = openBackend();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}},
			 { {"Embedding"}, {{"value", vectorValue(0.0, 1.0)}}}}));
	static_cast<void>(backend->createLogicalNodeVectorIndex({"embedding_l2", "Embedding", "value", 2U, "l2"}));
	auto exactSnapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(exactSnapshot, nullptr);
	LogicalGraphVectorSearchRequest malformed = exactRequest("embedding_l2");
	malformed.query_vector = {1.0F};
	EXPECT_THROW(static_cast<void>(exactSnapshot->searchLogicalVector(malformed)), std::invalid_argument);
	malformed.query_vector = {std::numeric_limits<float>::quiet_NaN(), 0.0F};
	EXPECT_THROW(static_cast<void>(exactSnapshot->searchLogicalVector(malformed)), std::invalid_argument);
	exactSnapshot.reset();

	ASSERT_TRUE(backend->trainLogicalVector({"embedding_l2", 16U, 16U}).trained);
	auto derivedSnapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(derivedSnapshot, nullptr);
	LogicalGraphVectorSearchRequest derived = exactRequest("embedding_l2");
	derived.top_k = 1U;
	derived.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	derived.candidate_refinement_budget = 2U;
	EXPECT_THROW(static_cast<void>(derivedSnapshot->searchLogicalVector(derived)), std::invalid_argument);
	derived.candidate_search_budget = 2U;
	derived.candidate_refinement_budget = 0U;
	EXPECT_THROW(static_cast<void>(derivedSnapshot->searchLogicalVector(derived)), std::invalid_argument);
	derived.candidate_refinement_budget = 2U;
	derived.candidate_search_budget = derived.maximum_mapping_entries + 1U;
	EXPECT_THROW(static_cast<void>(derivedSnapshot->searchLogicalVector(derived)), std::invalid_argument);
	derived.candidate_search_budget = 2U;
	derived.candidate_refinement_budget = derived.maximum_raw_vector_reads + 1U;
	EXPECT_THROW(static_cast<void>(derivedSnapshot->searchLogicalVector(derived)), std::invalid_argument);
	derived.candidate_refinement_budget = 1U;
	derived.top_k = 2U;
	EXPECT_THROW(static_cast<void>(derivedSnapshot->searchLogicalVector(derived)), std::invalid_argument);
	derivedSnapshot.reset();
	backend->close();
}
