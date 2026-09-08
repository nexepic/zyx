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

TEST_F(RelocatableStorageBackendVectorMetricTest, RejectsMalformedVectorSearchRequestsBeforeReadingPersistentState) {
	auto backend = openBackend();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}}}));
	static_cast<void>(backend->createLogicalNodeVectorIndex({"validated_embedding", "Embedding", "value", 2U, "l2"}));

	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const auto expectRejected = [&snapshot](LogicalGraphVectorSearchRequest request) {
		EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);
	};

	// These are public read contracts. Reject malformed requests before lookup or
	// scoring so a bad query cannot observe a partial persistent Vector state.
	LogicalGraphVectorSearchRequest request = exactRequest("validated_embedding");
	request.index_name.clear();
	expectRejected(request);
	request = exactRequest("validated_embedding");
	request.query_vector.clear();
	expectRejected(request);
	request = exactRequest("validated_embedding");
	request.top_k = 0U;
	expectRejected(request);
	request = exactRequest("validated_embedding");
	request.maximum_mapping_entries = 0U;
	expectRejected(request);
	request = exactRequest("validated_embedding");
	request.maximum_raw_vector_reads = 0U;
	expectRejected(request);
	request = exactRequest("validated_embedding");
	request.maximum_mapping_entries = std::numeric_limits<size_t>::max();
	expectRejected(request);
	request = exactRequest("validated_embedding");
	request.maximum_raw_vector_reads = std::numeric_limits<size_t>::max();
	expectRejected(request);
	request = exactRequest("validated_embedding");
	request.query_vector = {std::numeric_limits<float>::infinity(), 0.0F};
	expectRejected(request);
	request = exactRequest("validated_embedding");
	request.mode = static_cast<LogicalGraphVectorSearchMode>(999U);
	expectRejected(request);

	request = exactRequest("missing_embedding");
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	request = exactRequest("validated_embedding");
	request.query_vector = {1.0F, 0.0F, 0.0F};
	expectRejected(request);

	// The valid request still reaches exact scoring after all validation failures
	// above, proving the pinned snapshot remains usable.
	request = exactRequest("validated_embedding");
	const LogicalGraphVectorSearchResult valid = snapshot->searchLogicalVector(request);
	ASSERT_EQ(valid.hits.size(), 1U);
	EXPECT_EQ(valid.hits.front().node_id, 1U);
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

TEST_F(RelocatableStorageBackendVectorMetricTest, TrainsCandidatePrunedAdjacencyForSmallConfiguredIndexes) {
	RelocatableStorageBackendOptions candidateOptions = options();
	// A small threshold exercises the production candidate-pruned topology path
	// without turning this functional regression test into a scale workload.
	candidateOptions.maximum_vector_adjacency_exact_records = 1U;
	candidateOptions.vector_adjacency_candidate_search_budget = 4U;
	auto backend = RelocatableStorageBackend::initializeNew(std::move(candidateOptions));
	backend->open();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}},
			 { {"Embedding"}, {{"value", vectorValue(0.0, 1.0)}}},
			 { {"Embedding"}, {{"value", vectorValue(0.8, 0.2)}}},
			 { {"Embedding"}, {{"value", vectorValue(-1.0, 0.0)}}}});
	ASSERT_EQ(nodes.size(), 4U);
	static_cast<void>(backend->createLogicalNodeVectorIndex(
			{"candidate_pruned_embedding", "Embedding", "value", 2U, "l2"}));

	const LogicalGraphVectorTrainingResult trained =
			backend->trainLogicalVector({"candidate_pruned_embedding", 16U, 16U});
	ASSERT_TRUE(trained.trained);
	EXPECT_FALSE(trained.reused_existing_generation);
	EXPECT_EQ(trained.vector_count, nodes.size());

	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request = exactRequest("candidate_pruned_embedding");
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.candidate_search_budget = nodes.size();
	request.candidate_refinement_budget = nodes.size();
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), 1U);
	EXPECT_TRUE(result.stats.used_pq);
	EXPECT_EQ(result.hits.front().node_id, nodes.front());
	snapshot.reset();
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, RetrainingAnOnlineVectorReplacementReusesTheBaseCodebook) {
	auto backend = openBackend();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}},
			 { {"Embedding"}, {{"value", vectorValue(0.0, 1.0)}}},
			 { {"Embedding"}, {{"value", vectorValue(-1.0, 0.0)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	const LogicalGraphIndexDefinition index =
			backend->createLogicalNodeVectorIndex({"retrained_embedding", "Embedding", "value", 2U, "l2"});
	ASSERT_TRUE(backend->trainLogicalVector({index.name, 16U, 16U}).trained);

	// A regular online update must first become a delta. Retraining then folds
	// that delta into a new derived generation while retaining compatible PQ data.
	backend->replaceLogicalNodeProperties({{nodes.front(), {{"value", vectorValue(0.0, 0.5)}}}});
	std::unique_ptr<PreparedLogicalGraphVectorTraining> prepared =
			backend->prepareLogicalVectorTraining({index.name, 16U, 16U});
	ASSERT_NE(prepared, nullptr);
	EXPECT_TRUE(prepared->preview().will_train);
	const LogicalGraphVectorTrainingResult rebuilt = prepared->commit();
	EXPECT_TRUE(rebuilt.trained);
	EXPECT_FALSE(rebuilt.reused_existing_generation);
	EXPECT_EQ(rebuilt.vector_count, nodes.size());

	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request = exactRequest(index.name);
	request.query_vector = {0.0F, 0.5F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.candidate_search_budget = nodes.size();
	request.candidate_refinement_budget = nodes.size();
	const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
	ASSERT_EQ(result.hits.size(), 1U);
	EXPECT_EQ(result.hits.front().node_id, nodes.front());
	EXPECT_TRUE(result.stats.used_pq);
	EXPECT_FALSE(result.stats.used_delta);
	snapshot.reset();
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, RejectsTrainingThatExceedsItsRequestedRecordBoundWithoutPublishing) {
	auto backend = openBackend();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", vectorValue(1.0, 0.0)}}},
			 { {"Embedding"}, {{"value", vectorValue(0.0, 1.0)}}}}));
	static_cast<void>(backend->createLogicalNodeVectorIndex(
			{"bounded_training_embedding", "Embedding", "value", 2U, "l2"}));
	const std::vector<char> before = databaseBytes();

	EXPECT_THROW(static_cast<void>(backend->trainLogicalVector({"bounded_training_embedding", 1U, 16U})),
			 FormatException);
	EXPECT_EQ(databaseBytes(), before);

	const LogicalGraphVectorTrainingResult retried =
			backend->trainLogicalVector({"bounded_training_embedding", 16U, 16U});
	EXPECT_TRUE(retried.trained);
	EXPECT_EQ(retried.vector_count, 2U);
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, RejectsUnboundedVectorTrainingRequestsBeforePreparation) {
	auto backend = openBackend();
	static_cast<void>(backend->createLogicalNodeVectorIndex({"bounded_request_embedding", "Embedding", "value", 2U, "l2"}));
	const std::vector<char> before = databaseBytes();
	const auto rejectsWithoutPublication = [&](LogicalGraphVectorTrainingRequest request) {
		EXPECT_THROW(static_cast<void>(backend->prepareLogicalVectorTraining(request)), std::invalid_argument);
		EXPECT_EQ(databaseBytes(), before);
	};

	// All resource dimensions are part of the public maintenance contract. Reject
	// each malformed request before it can acquire or publish derived state.
	rejectsWithoutPublication({"", 16U, 16U});
	rejectsWithoutPublication({"missing", 0U, 16U});
	rejectsWithoutPublication({"missing", 16U, 0U});
	rejectsWithoutPublication({"missing", std::numeric_limits<size_t>::max(), 16U});
	rejectsWithoutPublication({"missing", 16U, std::numeric_limits<size_t>::max()});
	// Keep the index name valid for the remaining request-bound cases so each
	// public bound is evaluated instead of being short-circuited by name lookup.
	rejectsWithoutPublication({"bounded_request_embedding", 0U, 16U});
	rejectsWithoutPublication({"bounded_request_embedding", 16U, 0U});
	rejectsWithoutPublication(
			{"bounded_request_embedding", std::numeric_limits<size_t>::max(), 16U});
	rejectsWithoutPublication(
			{"bounded_request_embedding", 16U, std::numeric_limits<size_t>::max()});

	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, EmptyVectorIndexPreparesAndCommitsAsANoOp) {
	auto backend = openBackend();
	static_cast<void>(backend->createLogicalNodeVectorIndex({"empty_embedding", "Embedding", "value", 2U, "l2"}));
	const std::vector<char> before = databaseBytes();

	std::unique_ptr<PreparedLogicalGraphVectorTraining> prepared =
			backend->prepareLogicalVectorTraining({"empty_embedding", 16U, 16U});
	ASSERT_NE(prepared, nullptr);
	EXPECT_FALSE(prepared->preview().will_train);
	const LogicalGraphVectorTrainingResult result = prepared->commit();
	EXPECT_FALSE(result.trained);
	EXPECT_FALSE(result.reused_existing_generation);
	EXPECT_EQ(result.vector_count, 0U);
	EXPECT_EQ(databaseBytes(), before);

	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, PreparedVectorTrainingCannotCommitTwice) {
	auto backend = openBackend();
	static_cast<void>(backend->createLogicalNodeVectorIndex({"single_commit_embedding", "Embedding", "value", 2U, "l2"}));

	std::unique_ptr<PreparedLogicalGraphVectorTraining> prepared =
			backend->prepareLogicalVectorTraining({"single_commit_embedding", 16U, 16U});
	ASSERT_NE(prepared, nullptr);
	EXPECT_FALSE(prepared->preview().will_train);
	EXPECT_FALSE(prepared->commit().trained);
	EXPECT_THROW(static_cast<void>(prepared->commit()), std::logic_error);

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
	EXPECT_THROW(static_cast<void>(reader.prepareLogicalVectorTraining({"embedding_read_only", 16U, 16U})),
			 std::runtime_error);
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
	malformed = exactRequest("embedding_l2");
	malformed.maximum_raw_vector_reads = std::numeric_limits<size_t>::max();
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

TEST_F(RelocatableStorageBackendVectorMetricTest, DirectVectorDescriptorAndExactCandidateScoringHonorPublicContracts) {
	auto backend = openBackend();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"L2"}, {{"value", vectorValue(1.0, 0.0)}}},
			 {{"InnerProduct"}, {{"value", vectorValue(1.0, 2.0)}}},
			 {{"Cosine"}, {{"value", vectorValue(1.0, 0.0)}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend->createLogicalNodeVectorIndex({"l2_descriptor", "L2", "value", 2U, "l2"}));
	static_cast<void>(
			backend->createLogicalNodeVectorIndex({"ip_descriptor", "InnerProduct", "value", 2U, "ip"}));
	static_cast<void>(
			backend->createLogicalNodeVectorIndex({"cosine_descriptor", "Cosine", "value", 2U, "cosine"}));

	RelocatableReadSnapshot snapshot = backend->openSnapshot();
	const LogicalGraphVectorSearchRequest l2Request = exactRequest("l2_descriptor");
	const auto l2 = backend->describeLogicalVectorIndex(snapshot, l2Request);
	ASSERT_TRUE(l2.has_value());
	EXPECT_EQ(l2->label, "L2");
	EXPECT_EQ(l2->property_key, "value");
	EXPECT_EQ(l2->dimensions, 2U);
	EXPECT_EQ(l2->metric, "l2");

	const LogicalGraphNode l2Node = backend->readNode(snapshot, nodes.at(0));
	const auto l2Score = backend->scoreLogicalVectorExactOverlayCandidate(snapshot, l2Request, *l2, l2Node);
	ASSERT_TRUE(l2Score.has_value());
	EXPECT_EQ(l2Score->node_id, nodes.at(0));
	EXPECT_FLOAT_EQ(l2Score->score, 0.0F);
	EXPECT_EQ(backend->scoreLogicalVectorExactOverlayCandidate(
				  snapshot, l2Request, *l2, LogicalGraphNode(99U, {"Other"}, {})),
			  std::nullopt);
	EXPECT_EQ(backend->scoreLogicalVectorExactOverlayCandidate(
				  snapshot, l2Request, *l2, LogicalGraphNode(100U, {"L2"}, {})),
			  std::nullopt);

	const LogicalGraphVectorSearchRequest ipRequest = exactRequest("ip_descriptor");
	const auto ip = backend->describeLogicalVectorIndex(snapshot, ipRequest);
	ASSERT_TRUE(ip.has_value());
	const auto ipScore = backend->scoreLogicalVectorExactOverlayCandidate(
			snapshot, ipRequest, *ip, backend->readNode(snapshot, nodes.at(1)));
	ASSERT_TRUE(ipScore.has_value());
	EXPECT_FLOAT_EQ(ipScore->score, -1.0F);

	const LogicalGraphVectorSearchRequest cosineRequest = exactRequest("cosine_descriptor");
	const auto cosine = backend->describeLogicalVectorIndex(snapshot, cosineRequest);
	ASSERT_TRUE(cosine.has_value());
	const auto cosineScore = backend->scoreLogicalVectorExactOverlayCandidate(
			snapshot, cosineRequest, *cosine, backend->readNode(snapshot, nodes.at(2)));
	ASSERT_TRUE(cosineScore.has_value());
	EXPECT_FLOAT_EQ(cosineScore->score, 0.0F);
	EXPECT_THROW(static_cast<void>(backend->scoreLogicalVectorExactOverlayCandidate(
				snapshot, cosineRequest, *cosine,
				LogicalGraphNode(101U, {"Cosine"}, {{"value", vectorValue(0.0, 0.0)}}))),
			std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend->scoreLogicalVectorExactOverlayCandidate(
				snapshot, l2Request, *l2,
				LogicalGraphNode(102U, {"L2"}, {{"value", vectorValue(-std::numeric_limits<float>::max(), 0.0)}}))),
			std::overflow_error);

	LogicalGraphVectorIndexDescriptor invalidDescriptor = *l2;
	invalidDescriptor.index_id = 0U;
	EXPECT_THROW(static_cast<void>(backend->scoreLogicalVectorExactOverlayCandidate(
				snapshot, l2Request, invalidDescriptor, l2Node)),
			std::logic_error);
	invalidDescriptor = *l2;
	invalidDescriptor.metric = "unsupported";
	EXPECT_THROW(static_cast<void>(backend->scoreLogicalVectorExactOverlayCandidate(
				snapshot, l2Request, invalidDescriptor, l2Node)),
			std::logic_error);

	LogicalGraphVectorSearchRequest invalidRequest = l2Request;
	invalidRequest.mode = static_cast<LogicalGraphVectorSearchMode>(99U);
	EXPECT_THROW(static_cast<void>(backend->describeLogicalVectorIndex(snapshot, invalidRequest)), std::invalid_argument);
	invalidRequest = l2Request;
	invalidRequest.query_vector = {1.0F};
	EXPECT_THROW(static_cast<void>(backend->describeLogicalVectorIndex(snapshot, invalidRequest)), std::invalid_argument);
	const auto rejectsInvalidDescriptorRequest = [&](LogicalGraphVectorSearchRequest request) {
		EXPECT_THROW(static_cast<void>(backend->describeLogicalVectorIndex(snapshot, request)), std::invalid_argument);
	};
	rejectsInvalidDescriptorRequest({"", {1.0F, 0.0F}, 1U, LogicalGraphVectorSearchMode::LGVSM_EXACT, 16U, 16U});
	rejectsInvalidDescriptorRequest({"l2_descriptor", {}, 1U, LogicalGraphVectorSearchMode::LGVSM_EXACT, 16U, 16U});
	rejectsInvalidDescriptorRequest({"l2_descriptor", {1.0F, 0.0F}, 0U, LogicalGraphVectorSearchMode::LGVSM_EXACT,
								 16U, 16U});
	rejectsInvalidDescriptorRequest({"l2_descriptor", {1.0F, 0.0F}, 1U, LogicalGraphVectorSearchMode::LGVSM_EXACT,
								 0U, 16U});
	rejectsInvalidDescriptorRequest({"l2_descriptor", {1.0F, 0.0F}, 1U, LogicalGraphVectorSearchMode::LGVSM_EXACT,
								 16U, 0U});
	rejectsInvalidDescriptorRequest(
			{"l2_descriptor", {1.0F, 0.0F}, 1U, LogicalGraphVectorSearchMode::LGVSM_EXACT,
			 std::numeric_limits<size_t>::max(), 16U});
	rejectsInvalidDescriptorRequest(
			{"l2_descriptor", {1.0F, 0.0F}, 1U, LogicalGraphVectorSearchMode::LGVSM_EXACT,
			 16U, std::numeric_limits<size_t>::max()});
	rejectsInvalidDescriptorRequest(
			{"l2_descriptor", {std::numeric_limits<float>::quiet_NaN(), 0.0F}, 1U,
			 LogicalGraphVectorSearchMode::LGVSM_EXACT, 16U, 16U});
	EXPECT_THROW(static_cast<void>(backend->describeLogicalVectorIndex(
				snapshot, exactRequest("missing_descriptor"))),
			std::runtime_error);
	LogicalGraphVectorSearchRequest zeroCosineQuery = cosineRequest;
	zeroCosineQuery.query_vector = {0.0F, 0.0F};
	EXPECT_THROW(static_cast<void>(backend->describeLogicalVectorIndex(snapshot, zeroCosineQuery)), std::invalid_argument);

	snapshot.release();
	backend->close();
}

TEST_F(RelocatableStorageBackendVectorMetricTest, RejectsEveryMalformedExactOverlayDescriptorField) {
	auto backend = openBackend();
	RelocatableReadSnapshot snapshot = backend->openSnapshot();
	ASSERT_TRUE(snapshot.isValid());
	const LogicalGraphVectorSearchRequest request = exactRequest("descriptor");
	const LogicalGraphNode node(1U, {"Embedding"}, {});
	const LogicalGraphVectorIndexDescriptor valid{1U, "Embedding", "value", 2U, "l2"};
	const auto expectRejected = [&](LogicalGraphVectorIndexDescriptor descriptor,
								  const LogicalGraphVectorSearchRequest &candidateRequest) {
		EXPECT_THROW(static_cast<void>(backend->scoreLogicalVectorExactOverlayCandidate(
				snapshot, candidateRequest, descriptor, node)),
				 std::logic_error);
	};

	LogicalGraphVectorIndexDescriptor invalid = valid;
	invalid.label.clear();
	expectRejected(invalid, request);
	invalid = valid;
	invalid.property_key.clear();
	expectRejected(invalid, request);
	invalid = valid;
	invalid.dimensions = 0U;
	expectRejected(invalid, request);
	invalid = valid;
	LogicalGraphVectorSearchRequest wrongShape = request;
	wrongShape.query_vector = {1.0F};
	expectRejected(invalid, wrongShape);
	invalid = valid;
	invalid.metric = "unsupported";
	expectRejected(invalid, request);

	snapshot.release();
	backend->close();
}
