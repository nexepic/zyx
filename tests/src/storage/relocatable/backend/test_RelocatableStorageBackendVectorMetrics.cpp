/**
 * @file test_RelocatableStorageBackendVectorMetrics.cpp
 * @brief End-to-end metric semantics and cosine-domain validation tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <iterator>
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

	class ThrowAfterCheckpoint final : public CancellationCheckpoint {
	public:
		explicit ThrowAfterCheckpoint(size_t throwAfter) : throw_after_(throwAfter) {}

		void check() const override {
			++checks;
			if (checks == throw_after_) {
				throw std::runtime_error("test Vector cancellation checkpoint");
			}
		}

		mutable size_t checks = 0U;

	private:
		size_t throw_after_ = 0U;
	};

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

		[[nodiscard]] std::vector<char> databaseBytes() const {
			std::ifstream input(data_path_, std::ios::binary);
			EXPECT_TRUE(input.is_open());
			return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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

TEST_F(RelocatableStorageBackendVectorMetricTest, CancelledTrainingLeavesThePriorDerivedGenerationUnchanged) {
	auto backend = openBackend();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}},
			 { {"Embedding"}, {{"value", vectorValue(0.0, 1.0)}}},
			 { {"Embedding"}, {{"value", vectorValue(1.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend->createLogicalNodeVectorIndex({"cancelled_training", "Embedding", "value", 2U, "l2"}));
	const std::vector<char> before = databaseBytes();

	ThrowAfterCheckpoint cancellation(8U);
	EXPECT_THROW(static_cast<void>(backend->trainLogicalVector({"cancelled_training", 16U, 16U, &cancellation})),
			 std::runtime_error);
	EXPECT_EQ(databaseBytes(), before);

	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const LogicalGraphVectorSearchResult exact = snapshot->searchLogicalVector(exactRequest("cancelled_training"));
	EXPECT_EQ(exact.hits.size(), nodes.size());
	snapshot.reset();
	EXPECT_TRUE(backend->trainLogicalVector({"cancelled_training", 16U, 16U}).trained);
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, ExactSearchCancellationLeavesThePinnedGenerationReadable) {
	auto backend = openBackend();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}},
			 {{"Embedding"}, {{"value", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend->createLogicalNodeVectorIndex({"embedding_cancel", "Embedding", "value", 2U, "l2"}));

	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request = exactRequest("embedding_cancel");
	ThrowAfterCheckpoint cancellation(4U);
	request.cancellation = &cancellation;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	EXPECT_EQ(cancellation.checks, 4U);

	// A cancelled read never publishes a partial result or mutates the pinned
	// generation; the same snapshot remains readable after removing the guard.
	request.cancellation = nullptr;
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), nodes.size());
	EXPECT_EQ(result.hits.front().node_id, nodes.front());
	snapshot.reset();
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

TEST_F(RelocatableStorageBackendVectorMetricTest, TrainsAndQueriesAHighDimensionalPqIndex) {
	auto backend = openBackend();
	const auto vector = [](const size_t dimensions, const size_t hotCoordinate) {
		std::vector<graph::PropertyValue> values;
		values.reserve(dimensions);
		for (size_t coordinate = 0U; coordinate < dimensions; ++coordinate) {
			values.emplace_back(coordinate == hotCoordinate ? 1.0 : 0.0);
		}
		return graph::PropertyValue(std::move(values));
	};
	constexpr size_t dimensions = 24U;
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vector(dimensions, 0U)}}},
			 {{"Embedding"}, {{"value", vector(dimensions, 1U)}}},
			 {{"Embedding"}, {{"value", vector(dimensions, 2U)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend->createLogicalNodeVectorIndex(
			{"high_dimensional_embedding", "Embedding", "value", static_cast<uint32_t>(dimensions), "l2"}));

	const LogicalGraphVectorTrainingResult trained =
			backend->trainLogicalVector({"high_dimensional_embedding", 16U, 16U});
	ASSERT_TRUE(trained.trained);
	EXPECT_FALSE(trained.reused_existing_generation);
	EXPECT_EQ(trained.vector_count, nodes.size());

	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = "high_dimensional_embedding";
	request.query_vector.assign(dimensions, 0.0F);
	request.query_vector.front() = 1.0F;
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.maximum_mapping_entries = 16U;
	request.maximum_raw_vector_reads = 16U;
	request.candidate_search_budget = 3U;
	request.candidate_refinement_budget = 3U;
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), 1U);
	EXPECT_TRUE(result.stats.used_pq);
	EXPECT_EQ(result.hits.front().node_id, nodes.front());
	snapshot.reset();

	// A dimension that is not divisible by the preferred subspace count remains
	// valid: training falls back to one subspace instead of rejecting a legal
	// Vector schema or publishing a malformed codebook.
	constexpr size_t primeDimensions = 17U;
	const std::vector<uint64_t> primeNodes = backend->createLogicalNodes(
			{{{"PrimeEmbedding"}, {{"value", vector(primeDimensions, 0U)}}},
			 {{"PrimeEmbedding"}, {{"value", vector(primeDimensions, 1U)}}}});
	ASSERT_EQ(primeNodes.size(), 2U);
	static_cast<void>(backend->createLogicalNodeVectorIndex(
			{"prime_dimensional_embedding", "PrimeEmbedding", "value", static_cast<uint32_t>(primeDimensions), "l2"}));
	EXPECT_TRUE(backend->trainLogicalVector({"prime_dimensional_embedding", 16U, 16U}).trained);

	snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	request.index_name = "prime_dimensional_embedding";
	request.query_vector.assign(primeDimensions, 0.0F);
	request.query_vector.front() = 1.0F;
	request.top_k = 1U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	const LogicalGraphVectorSearchResult primeResult = snapshot->searchLogicalVector(request);
	ASSERT_EQ(primeResult.hits.size(), 1U);
	EXPECT_TRUE(primeResult.stats.used_pq);
	EXPECT_EQ(primeResult.hits.front().node_id, primeNodes.front());
	snapshot.reset();
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, ReadsTrainedAdjacencyInStrictReadOnlyModeWithoutChangingTheFile) {
	auto writer = openBackend();
	const std::vector<uint64_t> nodes = writer->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}},
			 {{"Embedding"}, {{"value", vectorValue(0.0, 1.0)}}},
			 {{"Embedding"}, {{"value", vectorValue(2.0, 0.0)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(writer->createLogicalNodeVectorIndex({"embedding_read_only", "Embedding", "value", 2U, "l2"}));
	ASSERT_TRUE(writer->trainLogicalVector({"embedding_read_only", 16U, 16U}).trained);
	writer->close();
	const std::vector<char> before = databaseBytes();
	ASSERT_FALSE(before.empty());

	RelocatableStorageBackendOptions readOnlyOptions = options();
	readOnlyOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	readOnlyOptions.verify_integrity_on_open = true;
	RelocatableStorageBackend reader(std::move(readOnlyOptions));
	ASSERT_NO_THROW(reader.open());
	auto snapshot = reader.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request = exactRequest("embedding_read_only");
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.candidate_search_budget = 3U;
	request.candidate_refinement_budget = 3U;
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), 1U);
	EXPECT_TRUE(result.stats.used_pq);
	EXPECT_EQ(result.hits.front().node_id, nodes.front());
	snapshot.reset();
	reader.close();

	EXPECT_EQ(databaseBytes(), before);
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
