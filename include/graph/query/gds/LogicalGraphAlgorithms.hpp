/**
 * @file LogicalGraphAlgorithms.hpp
 * @brief Bounded, storage-neutral graph algorithms over logical projection CSR.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "graph/query/gds/LogicalGraphProjection.hpp"

namespace graph::concurrent {
	class ThreadPool;
}

namespace graph::query::gds {
	struct LogicalGraphAlgorithmsTestAccess;

	struct LogicalGraphAlgorithmBounds {
		size_t maximum_nodes = 0U;
		size_t maximum_edges = 0U;
		size_t maximum_iterations = 0U;
		size_t maximum_work_units = 0U;
		// Zero keeps the storage-neutral direct-algorithm API unbounded by bytes;
		// query procedures pass their configured materialization limit.
		size_t maximum_materialized_bytes = 0U;
	};

	struct LogicalPageRankOptions {
		size_t maximum_iterations = 20U;
		double damping_factor = 0.85;
		double tolerance = 1.0e-6;
	};

	struct LogicalBetweennessOptions {
		// Zero computes the exact score from every Node. A positive value uses a
		// deterministic uniform sample so repeated runs have stable output.
		size_t sampling_size = 0U;
	};

	struct LogicalLeidenOptions {
		size_t maximum_iterations = 20U;
		size_t maximum_levels = 10U;
		double resolution = 1.0;
		double refinement_threshold = 0.01;
		// Zero lets the supplied database-owned pool choose its own bounded width.
		size_t concurrency = 0U;
	};

	struct LogicalGdsNodeScore {
		uint64_t node_id = 0U;
		double score = 0.0;

		bool operator==(const LogicalGdsNodeScore &) const = default;
	};

	struct LogicalGdsNodeComponent {
		uint64_t node_id = 0U;
		uint64_t component_id = 0U;

		bool operator==(const LogicalGdsNodeComponent &) const = default;
	};

	struct LogicalGdsNodeCommunity {
		uint64_t node_id = 0U;
		// The smallest original Node ID in the final community. This is stable for
		// a deterministic projection/result and is intentionally not a dense rank.
		uint64_t community_id = 0U;

		bool operator==(const LogicalGdsNodeCommunity &) const = default;
	};

	struct LogicalGdsWeightedPath {
		utils::OwnedStdVector<uint64_t> node_ids;
		// Same cardinality as node_ids. The first cost is zero; every subsequent
		// value is the chosen predecessor arc's weight.
		utils::OwnedStdVector<double> step_weights;
		double total_weight = 0.0;

		bool operator==(const LogicalGdsWeightedPath &) const = default;
	};

	// Algorithms call this at bounded intervals. The procedure runtime uses it to
	// enforce cancellation/deadline checks without coupling pure algorithms to a
	// query executor or a physical storage implementation.
	using LogicalGraphAlgorithmCheckpoint = std::function<void()>;

	class LogicalGraphAlgorithms final {
	public:
		[[nodiscard]] static std::vector<LogicalGdsNodeScore> pageRank(const LogicalGraphProjection &projection,
				LogicalPageRankOptions options, LogicalGraphAlgorithmBounds bounds,
				const LogicalGraphAlgorithmCheckpoint &checkpoint = {});
		[[nodiscard]] static std::vector<LogicalGdsNodeComponent> weaklyConnectedComponents(
				const LogicalGraphProjection &projection, LogicalGraphAlgorithmBounds bounds,
				const LogicalGraphAlgorithmCheckpoint &checkpoint = {});
		[[nodiscard]] static std::vector<LogicalGdsNodeScore> betweennessCentrality(
				const LogicalGraphProjection &projection, LogicalBetweennessOptions options,
				LogicalGraphAlgorithmBounds bounds, const LogicalGraphAlgorithmCheckpoint &checkpoint = {});
		[[nodiscard]] static std::vector<LogicalGdsNodeScore> closenessCentrality(
				const LogicalGraphProjection &projection, LogicalGraphAlgorithmBounds bounds,
				const LogicalGraphAlgorithmCheckpoint &checkpoint = {});
		[[nodiscard]] static std::vector<LogicalGdsNodeCommunity> leiden(
				const LogicalGraphProjection &projection, LogicalLeidenOptions options,
				LogicalGraphAlgorithmBounds bounds, const LogicalGraphAlgorithmCheckpoint &checkpoint = {},
				concurrent::ThreadPool *thread_pool = nullptr);
		[[nodiscard]] static double leidenModularity(const LogicalGraphProjection &projection,
				const std::vector<LogicalGdsNodeCommunity> &communities, double resolution = 1.0);
		[[nodiscard]] static LogicalGdsWeightedPath dijkstra(const LogicalGraphProjection &projection,
				uint64_t source_node_id, uint64_t target_node_id, LogicalGraphAlgorithmBounds bounds,
				const LogicalGraphAlgorithmCheckpoint &checkpoint = {});

        [[nodiscard]] static utils::OwnedStdVector<LogicalGdsNodeScore> pageRankOwned(const LogicalGraphProjection &projection,
				LogicalPageRankOptions options, LogicalGraphAlgorithmBounds bounds,
				const LogicalGraphAlgorithmCheckpoint &checkpoint = {});
        [[nodiscard]] static utils::OwnedStdVector<LogicalGdsNodeComponent> weaklyConnectedComponentsOwned(
				const LogicalGraphProjection &projection, LogicalGraphAlgorithmBounds bounds,
				const LogicalGraphAlgorithmCheckpoint &checkpoint = {});
        [[nodiscard]] static utils::OwnedStdVector<LogicalGdsNodeScore> betweennessCentralityOwned(
				const LogicalGraphProjection &projection, LogicalBetweennessOptions options,
				LogicalGraphAlgorithmBounds bounds, const LogicalGraphAlgorithmCheckpoint &checkpoint = {});
        [[nodiscard]] static utils::OwnedStdVector<LogicalGdsNodeScore> closenessCentralityOwned(
				const LogicalGraphProjection &projection, LogicalGraphAlgorithmBounds bounds,
				const LogicalGraphAlgorithmCheckpoint &checkpoint = {});
        [[nodiscard]] static utils::OwnedStdVector<LogicalGdsNodeCommunity> leidenOwned(
				const LogicalGraphProjection &projection, LogicalLeidenOptions options,
				LogicalGraphAlgorithmBounds bounds, const LogicalGraphAlgorithmCheckpoint &checkpoint = {},
				concurrent::ThreadPool *thread_pool = nullptr);

	private:
		friend struct LogicalGraphAlgorithmsTestAccess;

		using LogicalWeightedAdjacency = std::vector<std::vector<std::pair<size_t, double>>>;

		static void refineDisconnectedCommunities(const LogicalWeightedAdjacency &adjacency,
				std::vector<size_t> &community, const std::vector<double> &degree, double totalWeight,
				double resolution, double refinementThreshold, size_t &usedWork,
				const LogicalGraphAlgorithmBounds &bounds, const LogicalGraphAlgorithmCheckpoint &checkpoint);

		LogicalGraphAlgorithms() = delete;
	};

} // namespace graph::query::gds
