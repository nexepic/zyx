/**
 * @file LogicalGraphAlgorithms.cpp
 * @brief Bounded pure graph algorithms over native logical projections.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/gds/LogicalGraphAlgorithms.hpp"
#include "graph/concurrent/ThreadPool.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace graph::query::gds {

	namespace {

		struct ValidatedCsr {
			std::shared_ptr<const LogicalProjectionCsr> csr;
			std::unordered_map<uint64_t, size_t> node_to_index;
			size_t node_to_index_bytes = 0U;
		};

		// Checkpoints run on the calling thread because QueryGuard and other
		// procedure callbacks are not generally safe to invoke from pool workers.
		constexpr size_t LEIDEN_DEGREE_CHECKPOINT_NODE_STRIDE = 256U;

		void checkpoint(const LogicalGraphAlgorithmCheckpoint &check) {
			if (check) {
				check();
			}
		}

		void consumeWork(size_t &used, const size_t amount, const LogicalGraphAlgorithmBounds &bounds) {
			if (amount > bounds.maximum_work_units - used) {
				throw std::runtime_error("logical graph algorithm exceeded its work bound");
			}
			used += amount;
		}

		[[nodiscard]] ValidatedCsr requireWellFormedCsr(const LogicalGraphProjection &projection,
				const LogicalGraphAlgorithmCheckpoint &check) {
			std::shared_ptr<const LogicalProjectionCsr> csr = projection.csr();
			if (!csr || csr->offsets.size() != csr->node_ids.size() + 1U || csr->offsets.front() != 0U ||
				csr->offsets.back() != csr->edgeCount() || csr->weights.size() != csr->edgeCount() ||
				csr->relationship_ids.size() != csr->edgeCount() || csr->synthetic_reverse.size() != csr->edgeCount()) {
				throw std::runtime_error("logical graph algorithm received an invalid projection CSR");
			}
			ValidatedCsr result{.csr = std::move(csr)};
			result.node_to_index.reserve(result.csr->node_ids.size());
			uint64_t previous = 0U;
			for (size_t index = 0U; index < result.csr->node_ids.size(); ++index) {
				checkpoint(check);
				const uint64_t nodeId = result.csr->node_ids[index];
				if (nodeId == 0U || nodeId <= previous ||
					result.csr->offsets[index] > result.csr->offsets[index + 1U] ||
					result.csr->offsets[index + 1U] > result.csr->edgeCount()) {
					throw std::runtime_error("logical graph algorithm received malformed CSR node routing");
				}
				if (!result.node_to_index.emplace(nodeId, index).second) {
					throw std::runtime_error("logical graph algorithm received duplicate CSR Node ID");
				}
				previous = nodeId;
			}
			for (size_t edge = 0U; edge < result.csr->edgeCount(); ++edge) {
				checkpoint(check);
				if (!result.node_to_index.contains(result.csr->target_node_ids[edge]) || !std::isfinite(result.csr->weights[edge]) ||
					result.csr->weights[edge] < 0.0 || result.csr->synthetic_reverse[edge] > uint8_t{1U}) {
					throw std::runtime_error("logical graph algorithm received malformed CSR edge data");
				}
			}
			return result;
		}

		[[nodiscard]] ValidatedCsr requireValidCsr(const LogicalGraphProjection &projection,
				const LogicalGraphAlgorithmBounds &bounds, const char *materializedBoundError,
				const LogicalGraphAlgorithmCheckpoint &check) {
			if (bounds.maximum_nodes == 0U || bounds.maximum_edges == 0U || bounds.maximum_iterations == 0U ||
				bounds.maximum_work_units == 0U) {
				throw std::invalid_argument("logical graph algorithms require positive resource bounds");
			}
			// Reject oversized projections before materializing their CSR cache or
			// allocating the per-Node routing hash table used by every algorithm.
			const size_t nodeCount = projection.nodeCount();
			const size_t edgeCount = projection.edgeCount();
			if (nodeCount > bounds.maximum_nodes || edgeCount > bounds.maximum_edges) {
				throw std::runtime_error("logical graph algorithm received an invalid projection CSR");
			}
			constexpr size_t nodeIndexBytesPerEntry =
					sizeof(std::pair<const uint64_t, size_t>) + 3U * sizeof(void *);
			if (nodeCount > std::numeric_limits<size_t>::max() / nodeIndexBytesPerEntry) {
				throw std::runtime_error("logical graph algorithm work estimate overflowed its bound");
			}
			const size_t nodeIndexBytes = nodeCount * nodeIndexBytesPerEntry;
			if (bounds.maximum_materialized_bytes != 0U && nodeIndexBytes > bounds.maximum_materialized_bytes) {
				throw std::runtime_error(materializedBoundError);
			}
			checkpoint(check);
			ValidatedCsr result = requireWellFormedCsr(projection, check);
			result.node_to_index_bytes = nodeIndexBytes;
			return result;
		}

		[[nodiscard]] size_t checkedMultiply(const size_t left, const size_t right, const size_t maximum) {
			if (left != 0U && right > maximum / left) {
				throw std::runtime_error("logical graph algorithm work estimate overflowed its bound");
			}
			return left * right;
		}

		[[nodiscard]] size_t checkedAdd(const size_t left, const size_t right, const size_t maximum) {
			if (left > maximum || right > maximum - left) {
				throw std::runtime_error("logical graph algorithm work estimate overflowed its bound");
			}
			return left + right;
		}

		void requireMaterializedBytes(const size_t bytes, const LogicalGraphAlgorithmBounds &bounds,
								  const char *algorithm) {
			if (bounds.maximum_materialized_bytes != 0U && bytes > bounds.maximum_materialized_bytes) {
				throw std::runtime_error(std::string("logical ") + algorithm +
						" exceeded its materialized byte bound");
			}
		}

		[[nodiscard]] std::vector<std::vector<size_t>>
		buildUndirectedAdjacency(const ValidatedCsr &view, size_t &usedWork, const LogicalGraphAlgorithmBounds &bounds,
								 const LogicalGraphAlgorithmCheckpoint &check) {
			std::vector<std::vector<size_t>> adjacency(view.csr->node_ids.size());
			for (size_t source = 0U; source < adjacency.size(); ++source) {
				checkpoint(check);
				for (size_t edge = view.csr->offsets[source]; edge < view.csr->offsets[source + 1U]; ++edge) {
					// An UNDIRECTED projection already materializes its reverse arc. The
					// algorithms below derive their own symmetric view, so consuming that
					// synthetic arc again would turn one relationship into two edges.
					if (view.csr->synthetic_reverse[edge] != uint8_t{0U}) {
						continue;
					}
					const size_t target = view.node_to_index.at(view.csr->target_node_ids[edge]);
					adjacency[source].push_back(target);
					if (target != source) {
						adjacency[target].push_back(source);
					}
					consumeWork(usedWork, target == source ? 1U : 2U, bounds);
				}
			}
			return adjacency;
		}

		[[nodiscard]] std::vector<std::vector<std::pair<size_t, double>>>
		buildUndirectedWeightedAdjacency(const ValidatedCsr &view, size_t &usedWork,
										 const LogicalGraphAlgorithmBounds &bounds,
										 const LogicalGraphAlgorithmCheckpoint &check) {
			std::vector<std::vector<std::pair<size_t, double>>> adjacency(view.csr->node_ids.size());
			for (size_t source = 0U; source < adjacency.size(); ++source) {
				checkpoint(check);
				for (size_t edge = view.csr->offsets[source]; edge < view.csr->offsets[source + 1U]; ++edge) {
					// See buildUndirectedAdjacency(). Preserve two self-loop incidences,
					// but only one symmetric pair for every non-self relationship.
					if (view.csr->synthetic_reverse[edge] != uint8_t{0U}) {
						continue;
					}
					const size_t target = view.node_to_index.at(view.csr->target_node_ids[edge]);
					const double weight = view.csr->weights[edge];
					adjacency[source].emplace_back(target, weight);
					adjacency[target].emplace_back(source, weight);
					consumeWork(usedWork, 2U, bounds);
				}
			}
			return adjacency;
		}

		void sortScores(std::vector<LogicalGdsNodeScore> &scores) {
			std::ranges::sort(scores, [](const LogicalGdsNodeScore &left, const LogicalGdsNodeScore &right) {
				return left.score != right.score ? left.score > right.score : left.node_id < right.node_id;
			});
		}

	} // namespace

	std::vector<LogicalGdsNodeScore> LogicalGraphAlgorithms::pageRank(const LogicalGraphProjection &projection,
			const LogicalPageRankOptions options, const LogicalGraphAlgorithmBounds bounds,
			const LogicalGraphAlgorithmCheckpoint &check) {
		const ValidatedCsr view = requireValidCsr(projection, bounds,
				"logical PageRank exceeded its materialized byte bound", check);
		const size_t nodeCount = view.csr->node_ids.size();
		if (nodeCount == 0U) {
			return {};
		}
		if (options.maximum_iterations == 0U || options.maximum_iterations > bounds.maximum_iterations ||
			!std::isfinite(options.damping_factor) || options.damping_factor < 0.0 || options.damping_factor > 1.0 ||
			!std::isfinite(options.tolerance) || options.tolerance <= 0.0) {
			throw std::invalid_argument("logical PageRank options are invalid or exceed their bounds");
		}
		static_cast<void>(checkedMultiply(nodeCount, options.maximum_iterations, bounds.maximum_work_units));
		const size_t bytesPerNode = sizeof(double) * 2U + sizeof(LogicalGdsNodeScore);
		requireMaterializedBytes(checkedAdd(view.node_to_index_bytes,
				checkedMultiply(nodeCount, bytesPerNode, std::numeric_limits<size_t>::max()),
				std::numeric_limits<size_t>::max()),
				bounds, "PageRank");
		std::vector<double> scores(nodeCount, 1.0 / static_cast<double>(nodeCount));
		std::vector<double> next(nodeCount, 0.0);
		const double teleport = (1.0 - options.damping_factor) / static_cast<double>(nodeCount);
		size_t usedWork = 0U;
		for (size_t iteration = 0U; iteration < options.maximum_iterations; ++iteration) {
			checkpoint(check);
			std::fill(next.begin(), next.end(), 0.0);
			double dangling = 0.0;
			for (size_t source = 0U; source < nodeCount; ++source) {
				const size_t begin = view.csr->offsets[source];
				const size_t end = view.csr->offsets[source + 1U];
				if (begin == end) {
					dangling += scores[source];
					continue;
				}
				const double contribution = options.damping_factor * scores[source] /
						static_cast<double>(end - begin);
				for (size_t edge = begin; edge < end; ++edge) {
					next[view.node_to_index.at(view.csr->target_node_ids[edge])] += contribution;
					consumeWork(usedWork, 1U, bounds);
				}
			}
			const double danglingContribution = options.damping_factor * dangling / static_cast<double>(nodeCount);
			double difference = 0.0;
			for (size_t node = 0U; node < nodeCount; ++node) {
				next[node] += teleport + danglingContribution;
				difference += std::abs(next[node] - scores[node]);
				consumeWork(usedWork, 1U, bounds);
			}
			scores.swap(next);
			if (difference <= options.tolerance) {
				break;
			}
		}
		std::vector<LogicalGdsNodeScore> result;
		result.reserve(nodeCount);
		for (size_t index = 0U; index < nodeCount; ++index) {
			result.push_back({view.csr->node_ids[index], scores[index]});
		}
		sortScores(result);
		return result;
	}

	std::vector<LogicalGdsNodeComponent> LogicalGraphAlgorithms::weaklyConnectedComponents(
			const LogicalGraphProjection &projection, const LogicalGraphAlgorithmBounds bounds,
			const LogicalGraphAlgorithmCheckpoint &check) {
		const ValidatedCsr view = requireValidCsr(projection, bounds,
				"logical WCC exceeded its materialized byte bound", check);
		const size_t nodeCount = view.csr->node_ids.size();
		if (nodeCount == 0U) {
			return {};
		}
		if (view.csr->edgeCount() > bounds.maximum_work_units) {
			throw std::runtime_error("logical WCC exceeds its configured work bound");
		}
		const size_t bytesPerNode = sizeof(size_t) + sizeof(LogicalGdsNodeComponent);
		requireMaterializedBytes(checkedAdd(view.node_to_index_bytes,
				checkedMultiply(nodeCount, bytesPerNode, std::numeric_limits<size_t>::max()),
				std::numeric_limits<size_t>::max()),
				bounds, "WCC");
		std::vector<size_t> parent(nodeCount);
		std::iota(parent.begin(), parent.end(), 0U);
		const auto find = [&parent](size_t node) {
			size_t root = node;
			while (parent[root] != root) {
				root = parent[root];
			}
			while (parent[node] != node) {
				const size_t next = parent[node];
				parent[node] = root;
				node = next;
			}
			return root;
		};
		size_t usedWork = 0U;
		for (size_t source = 0U; source < nodeCount; ++source) {
			checkpoint(check);
			for (size_t edge = view.csr->offsets[source]; edge < view.csr->offsets[source + 1U]; ++edge) {
				const size_t sourceRoot = find(source);
				const size_t targetRoot = find(view.node_to_index.at(view.csr->target_node_ids[edge]));
				if (sourceRoot != targetRoot) {
					// Node IDs are sorted, so making the lower root authoritative gives
					// a canonical component ID without a second component-minimum pass.
					parent[std::max(sourceRoot, targetRoot)] = std::min(sourceRoot, targetRoot);
				}
				consumeWork(usedWork, 1U, bounds);
			}
		}
		std::vector<LogicalGdsNodeComponent> result;
		result.reserve(nodeCount);
		for (size_t index = 0U; index < nodeCount; ++index) {
			result.push_back({view.csr->node_ids[index], view.csr->node_ids[find(index)]});
		}
		return result;
	}

	std::vector<LogicalGdsNodeScore> LogicalGraphAlgorithms::betweennessCentrality(
			const LogicalGraphProjection &projection, const LogicalBetweennessOptions options,
			const LogicalGraphAlgorithmBounds bounds, const LogicalGraphAlgorithmCheckpoint &check) {
		const ValidatedCsr view = requireValidCsr(projection, bounds,
				"logical Betweenness exceeded its materialized byte bound", check);
		const size_t nodeCount = view.csr->node_ids.size();
		if (nodeCount == 0U) {
			return {};
		}
		const size_t maximumSize = std::numeric_limits<size_t>::max();
		const size_t perNodeBytes = checkedAdd(sizeof(size_t) + sizeof(std::vector<size_t>),
				checkedAdd(sizeof(double) * 3U + sizeof(size_t) * 3U,
						sizeof(LogicalGdsNodeScore), maximumSize), maximumSize);
		// Both adjacency and predecessor vectors can retain capacity above their
		// logical degree. Reserve a 2x growth margin for each undirected arc set.
		const size_t stateBytes = checkedAdd(view.node_to_index_bytes,
				checkedAdd(checkedMultiply(nodeCount, perNodeBytes, maximumSize),
						checkedMultiply(view.csr->edgeCount(), sizeof(size_t) * 8U, maximumSize), maximumSize), maximumSize);
		requireMaterializedBytes(stateBytes, bounds, "Betweenness");
		std::vector<size_t> sources(nodeCount);
		std::iota(sources.begin(), sources.end(), 0U);
		if (options.sampling_size != 0U && options.sampling_size < nodeCount) {
			std::mt19937_64 generator(42U);
			std::shuffle(sources.begin(), sources.end(), generator);
			sources.resize(options.sampling_size);
		}
		const size_t perSourceEstimate = checkedAdd(nodeCount,
				checkedMultiply(view.csr->edgeCount(), 6U, bounds.maximum_work_units), bounds.maximum_work_units);
		static_cast<void>(checkedMultiply(sources.size(), perSourceEstimate, bounds.maximum_work_units));
		size_t usedWork = 0U;
		const std::vector<std::vector<size_t>> adjacency = buildUndirectedAdjacency(view, usedWork, bounds, check);
		std::vector<double> centrality(nodeCount, 0.0);
		std::vector<double> sigma(nodeCount, 0.0);
		std::vector<double> dependency(nodeCount, 0.0);
		std::vector<size_t> distance(nodeCount, nodeCount);
		std::vector<std::vector<size_t>> predecessors(nodeCount);
		std::vector<size_t> queue;
		std::vector<size_t> stack;
		queue.reserve(nodeCount);
		stack.reserve(nodeCount);
		for (const size_t source: sources) {
			checkpoint(check);
			std::fill(sigma.begin(), sigma.end(), 0.0);
			std::fill(dependency.begin(), dependency.end(), 0.0);
			std::fill(distance.begin(), distance.end(), nodeCount);
			for (std::vector<size_t> &entries: predecessors) {
				entries.clear();
			}
			consumeWork(usedWork, nodeCount, bounds);
			queue.clear();
			stack.clear();
			queue.push_back(source);
			distance[source] = 0U;
			sigma[source] = 1.0;
			for (size_t head = 0U; head < queue.size(); ++head) {
				checkpoint(check);
				const size_t current = queue[head];
				stack.push_back(current);
				for (const size_t next: adjacency[current]) {
					if (distance[next] == nodeCount) {
						distance[next] = distance[current] + 1U;
						queue.push_back(next);
					}
					if (distance[next] == distance[current] + 1U) {
						sigma[next] += sigma[current];
						predecessors[next].push_back(current);
					}
					consumeWork(usedWork, 1U, bounds);
				}
			}
			while (!stack.empty()) {
				const size_t current = stack.back();
				stack.pop_back();
				for (const size_t predecessor: predecessors[current]) {
					if (sigma[current] == 0.0 || !std::isfinite(sigma[current])) {
						throw std::runtime_error("logical Betweenness encountered an invalid shortest-path count");
					}
					dependency[predecessor] += (sigma[predecessor] / sigma[current]) * (1.0 + dependency[current]);
					consumeWork(usedWork, 1U, bounds);
				}
				if (current != source) {
					centrality[current] += dependency[current];
				}
			}
		}
		const double normalization = options.sampling_size != 0U && options.sampling_size < nodeCount
				? static_cast<double>(nodeCount) / static_cast<double>(sources.size() * 2U)
				: 0.5;
		std::vector<LogicalGdsNodeScore> result;
		result.reserve(nodeCount);
		for (size_t index = 0U; index < nodeCount; ++index) {
			const double score = centrality[index] * normalization;
			if (!std::isfinite(score) || score < 0.0) {
				throw std::runtime_error("logical Betweenness produced an invalid score");
			}
			result.push_back({view.csr->node_ids[index], score});
		}
		sortScores(result);
		return result;
	}

	std::vector<LogicalGdsNodeScore> LogicalGraphAlgorithms::closenessCentrality(
			const LogicalGraphProjection &projection, const LogicalGraphAlgorithmBounds bounds,
			const LogicalGraphAlgorithmCheckpoint &check) {
		const ValidatedCsr view = requireValidCsr(projection, bounds,
				"logical Closeness exceeded its materialized byte bound", check);
		const size_t nodeCount = view.csr->node_ids.size();
		if (nodeCount == 0U) {
			return {};
		}
		const size_t perSourceEstimate = checkedAdd(nodeCount,
				checkedMultiply(view.csr->edgeCount(), 2U, bounds.maximum_work_units), bounds.maximum_work_units);
		const size_t traversalEstimate = checkedMultiply(nodeCount, perSourceEstimate, bounds.maximum_work_units);
		static_cast<void>(checkedAdd(checkedMultiply(view.csr->edgeCount(), 2U, bounds.maximum_work_units),
				traversalEstimate, bounds.maximum_work_units));
		const size_t maximumSize = std::numeric_limits<size_t>::max();
		const size_t perNodeBytes = checkedAdd(sizeof(std::vector<size_t>),
				checkedAdd(sizeof(size_t) * 4U, sizeof(LogicalGdsNodeScore) * 2U, maximumSize), maximumSize);
		const size_t stateBytes = checkedAdd(view.node_to_index_bytes,
				checkedAdd(checkedMultiply(nodeCount, perNodeBytes, maximumSize),
						checkedMultiply(view.csr->edgeCount(), sizeof(size_t) * 4U, maximumSize), maximumSize), maximumSize);
		requireMaterializedBytes(stateBytes, bounds, "Closeness");
		size_t usedWork = 0U;
		const std::vector<std::vector<size_t>> adjacency = buildUndirectedAdjacency(view, usedWork, bounds, check);
		std::vector<size_t> distance(nodeCount, nodeCount);
		std::vector<size_t> queue;
		queue.reserve(nodeCount);
		std::vector<LogicalGdsNodeScore> result;
		result.reserve(nodeCount);
		for (size_t source = 0U; source < nodeCount; ++source) {
			checkpoint(check);
			std::fill(distance.begin(), distance.end(), nodeCount);
			consumeWork(usedWork, nodeCount, bounds);
			queue.clear();
			queue.push_back(source);
			distance[source] = 0U;
			size_t totalDistance = 0U;
			size_t reachable = 0U;
			for (size_t head = 0U; head < queue.size(); ++head) {
				checkpoint(check);
				const size_t current = queue[head];
				for (const size_t next: adjacency[current]) {
					if (distance[next] == nodeCount) {
						distance[next] = distance[current] + 1U;
						if (distance[next] > std::numeric_limits<size_t>::max() - totalDistance) {
							throw std::runtime_error("logical Closeness distance accumulation overflowed");
						}
						totalDistance += distance[next];
						++reachable;
						queue.push_back(next);
					}
					consumeWork(usedWork, 1U, bounds);
				}
			}
			const double score = reachable == 0U || totalDistance == 0U ? 0.0 :
					static_cast<double>(reachable) / static_cast<double>(totalDistance);
			result.push_back({view.csr->node_ids[source], score});
		}
		sortScores(result);
		return result;
	}

	std::vector<LogicalGdsNodeCommunity> LogicalGraphAlgorithms::leiden(
			const LogicalGraphProjection &projection, const LogicalLeidenOptions options,
			const LogicalGraphAlgorithmBounds bounds, const LogicalGraphAlgorithmCheckpoint &check,
			concurrent::ThreadPool *threadPool) {
		const ValidatedCsr view = requireValidCsr(projection, bounds,
				"logical Leiden exceeded its materialized byte bound", check);
		const size_t nodeCount = view.csr->node_ids.size();
		if (nodeCount == 0U) {
			return {};
		}
		if (options.maximum_iterations == 0U || options.maximum_iterations > bounds.maximum_iterations ||
			options.maximum_levels == 0U || !std::isfinite(options.resolution) || options.resolution <= 0.0 ||
			!std::isfinite(options.refinement_threshold) || options.refinement_threshold < 0.0) {
			throw std::invalid_argument("logical Leiden options are invalid or exceed their bounds");
		}
		// Leiden's level-local maps and refinement partitions coexist with both
		// the current and contracted weighted adjacency. Admit a conservative
		// envelope before building any of those retained structures.
		constexpr size_t LEIDEN_NODE_STATE_ESTIMATE_BYTES = 512U;
		constexpr size_t LEIDEN_EDGE_STATE_ESTIMATE_BYTES = 512U;
		const size_t maximumSize = std::numeric_limits<size_t>::max();
		const size_t estimatedBytes = checkedAdd(
				checkedMultiply(nodeCount, LEIDEN_NODE_STATE_ESTIMATE_BYTES, maximumSize),
				checkedMultiply(view.csr->edgeCount(), LEIDEN_EDGE_STATE_ESTIMATE_BYTES, maximumSize), maximumSize);
		requireMaterializedBytes(checkedAdd(view.node_to_index_bytes, estimatedBytes,
				std::numeric_limits<size_t>::max()), bounds, "Leiden");
		size_t usedWork = 0U;
		using WeightedAdjacency = std::vector<std::vector<std::pair<size_t, double>>>;
		const auto denseLabels = [](std::vector<size_t> &labels) {
			std::map<size_t, size_t> remap;
			for (size_t &label: labels) {
				const auto [found, inserted] = remap.emplace(label, remap.size());
				static_cast<void>(inserted);
				label = found->second;
			}
			return remap.size();
		};
		const auto contract = [&bounds, &check, &usedWork](const WeightedAdjacency &adjacency,
														   const std::vector<size_t> &labels,
														   const size_t communityCount) {
			std::map<std::pair<size_t, size_t>, double> aggregate;
			for (size_t source = 0U; source < adjacency.size(); ++source) {
				checkpoint(check);
				for (const auto &[target, weight]: adjacency[source]) {
					consumeWork(usedWork, 1U, bounds);
					if (source > target) {
						continue;
					}
					const auto key = std::minmax(labels[source], labels[target]);
					aggregate[key] += source == target ? weight * 0.5 : weight;
				}
			}
			WeightedAdjacency result(communityCount);
			for (const auto &[key, weight]: aggregate) {
				consumeWork(usedWork, 1U, bounds);
				if (key.first == key.second) {
					result[key.first].emplace_back(key.first, weight);
					result[key.first].emplace_back(key.first, weight);
				} else {
					result[key.first].emplace_back(key.second, weight);
					result[key.second].emplace_back(key.first, weight);
				}
			}
			return result;
		};
		WeightedAdjacency adjacency = buildUndirectedWeightedAdjacency(view, usedWork, bounds, check);
		std::vector<size_t> originalToCurrent(nodeCount);
		std::iota(originalToCurrent.begin(), originalToCurrent.end(), 0U);
		for (size_t level = 0U; level < options.maximum_levels; ++level) {
			checkpoint(check);
			const size_t currentNodeCount = adjacency.size();
			std::vector<double> degree(currentNodeCount, 0.0);
			double totalWeight = 0.0;
			size_t degreeWork = 0U;
			for (const auto &neighbors: adjacency) {
				if (neighbors.size() > bounds.maximum_work_units - degreeWork) {
					throw std::runtime_error("logical Leiden degree scan exceeded its work bound");
				}
				degreeWork += neighbors.size();
			}
			const size_t workerLimit = threadPool == nullptr ? 1U :
					(options.concurrency == 0U ? threadPool->getThreadCount() :
						std::min(options.concurrency, threadPool->getThreadCount()));
			const auto computeDegree = [&](const size_t node) {
				for (const auto &[ignored, weight]: adjacency[node]) {
					static_cast<void>(ignored);
					degree[node] += weight;
				}
			};
			for (size_t begin = 0U; begin < currentNodeCount;) {
				checkpoint(check);
				const size_t batchSize = std::min(LEIDEN_DEGREE_CHECKPOINT_NODE_STRIDE, currentNodeCount - begin);
				const size_t end = begin + batchSize;
				if (threadPool != nullptr && workerLimit > 1U) {
					threadPool->parallelFor(begin, end, workerLimit, computeDegree);
				} else {
					for (size_t node = begin; node < end; ++node) computeDegree(node);
				}
				begin = end;
			}
			consumeWork(usedWork, degreeWork, bounds);
			for (const double value: degree) totalWeight += value;
			if (!std::isfinite(totalWeight)) {
				throw std::runtime_error("logical Leiden total relationship weight overflowed");
			}
			std::vector<size_t> community(currentNodeCount);
			std::iota(community.begin(), community.end(), 0U);
			if (totalWeight > 0.0) {
				const auto modularity = [&](const std::vector<size_t> &labels) {
					std::unordered_map<size_t, double> degreeByCommunity;
					double internalWeight = 0.0;
					for (size_t source = 0U; source < currentNodeCount; ++source) {
						degreeByCommunity[labels[source]] += degree[source];
						for (const auto &[target, weight]: adjacency[source]) {
							if (labels[source] == labels[target]) internalWeight += weight;
						}
					}
					double result = internalWeight / totalWeight;
					for (const auto &[ignored, communityDegree]: degreeByCommunity) {
						static_cast<void>(ignored);
						result -= options.resolution * (communityDegree / totalWeight) *
								(communityDegree / totalWeight);
					}
					return result;
				};
				std::vector<double> communityDegree = degree;
				for (size_t iteration = 0U; iteration < options.maximum_iterations; ++iteration) {
					checkpoint(check);
					const std::vector<size_t> beforeIteration = community;
					const double beforeModularity = modularity(beforeIteration);
					bool moved = false;
					for (size_t node = 0U; node < currentNodeCount; ++node) {
						std::unordered_map<size_t, double> weightByCommunity;
						for (const auto &[neighbor, weight]: adjacency[node]) {
							if (neighbor != node) weightByCommunity[community[neighbor]] += weight;
							consumeWork(usedWork, 1U, bounds);
						}
						const size_t current = community[node];
						const double currentScore = weightByCommunity[current] - options.resolution * degree[node] *
								(communityDegree[current] - degree[node]) / totalWeight;
						size_t best = current;
						double bestScore = currentScore;
						for (const auto &[candidate, weight]: weightByCommunity) {
							if (candidate == current) continue;
							const double score = weight - options.resolution * degree[node] * communityDegree[candidate] / totalWeight;
							if (score > bestScore + 1.0e-12 ||
								(std::abs(score - bestScore) <= 1.0e-12 && candidate < best)) {
								best = candidate;
								bestScore = score;
							}
						}
						if (best != current && bestScore > currentScore + 1.0e-12) {
							communityDegree[current] -= degree[node];
							communityDegree[best] += degree[node];
							community[node] = best;
							moved = true;
						}
					}
					if (modularity(community) + 1.0e-12 < beforeModularity) {
						community = beforeIteration;
						break;
					}
					if (!moved) break;
				}
			}
			refineDisconnectedCommunities(adjacency, community, degree, totalWeight, options.resolution,
					options.refinement_threshold, usedWork, bounds, check);
			const size_t communityCount = denseLabels(community);
			for (size_t &current: originalToCurrent) current = community[current];
			if (communityCount <= 1U || communityCount >= currentNodeCount || level + 1U == options.maximum_levels) break;
			adjacency = contract(adjacency, community, communityCount);
		}
		std::vector<uint64_t> canonical(adjacency.size(), std::numeric_limits<uint64_t>::max());
		for (size_t node = 0U; node < nodeCount; ++node) {
			canonical[originalToCurrent[node]] = std::min(canonical[originalToCurrent[node]], view.csr->node_ids[node]);
		}
		std::vector<LogicalGdsNodeCommunity> result;
		result.reserve(nodeCount);
		for (size_t node = 0U; node < nodeCount; ++node) {
			result.push_back({view.csr->node_ids[node], canonical[originalToCurrent[node]]});
		}
		return result;
	}

	void LogicalGraphAlgorithms::refineDisconnectedCommunities(const LogicalWeightedAdjacency &adjacency,
			std::vector<size_t> &community, const std::vector<double> &degree, const double totalWeight,
			const double resolution, const double refinementThreshold, size_t &usedWork,
			const LogicalGraphAlgorithmBounds &bounds, const LogicalGraphAlgorithmCheckpoint &check) {
		if (refinementThreshold == 0.0 || community.empty() || totalWeight == 0.0) {
			return;
		}
		if (adjacency.size() != community.size() || degree.size() != community.size() || !std::isfinite(totalWeight) ||
			totalWeight < 0.0 || !std::isfinite(resolution) || resolution <= 0.0 ||
			!std::isfinite(refinementThreshold) || refinementThreshold < 0.0) {
			throw std::invalid_argument("logical Leiden refinement received invalid input");
		}
		const size_t nodeCount = community.size();
		std::vector<size_t> componentByNode(nodeCount, nodeCount);
		std::vector<std::vector<size_t>> components;
		for (size_t seed = 0U; seed < nodeCount; ++seed) {
			if (componentByNode[seed] != nodeCount) continue;
			const size_t component = components.size();
			std::vector<size_t> queue{seed};
			componentByNode[seed] = component;
			components.emplace_back();
			for (size_t head = 0U; head < queue.size(); ++head) {
				checkpoint(check);
				const size_t current = queue[head];
				components.back().push_back(current);
				for (const auto &[neighbor, weight]: adjacency[current]) {
					if (neighbor >= nodeCount || !std::isfinite(weight) || weight < 0.0) {
						throw std::invalid_argument("logical Leiden refinement received malformed adjacency");
					}
					if (weight > 0.0 && community[neighbor] == community[seed] &&
						componentByNode[neighbor] == nodeCount) {
						componentByNode[neighbor] = component;
						queue.push_back(neighbor);
					}
					consumeWork(usedWork, 1U, bounds);
				}
			}
		}
		std::map<size_t, std::vector<size_t>> componentsByCommunity;
		for (size_t component = 0U; component < components.size(); ++component) {
			componentsByCommunity[community[components[component].front()]].push_back(component);
		}
		std::vector<size_t> refined(nodeCount);
		for (const auto &[communityId, componentIds]: componentsByCommunity) {
			static_cast<void>(communityId);
			std::vector<double> componentDegrees(componentIds.size(), 0.0);
			size_t corePosition = 0U;
			for (size_t position = 0U; position < componentIds.size(); ++position) {
				const std::vector<size_t> &component = components[componentIds[position]];
				for (const size_t node: component) componentDegrees[position] += degree[node];
				if (component.size() > components[componentIds[corePosition]].size()) corePosition = position;
			}
			double totalCommunityDegree = 0.0;
			for (const double componentDegree: componentDegrees) totalCommunityDegree += componentDegree;
			const size_t coreComponent = componentIds[corePosition];
			for (size_t position = 0U; position < componentIds.size(); ++position) {
				const size_t component = componentIds[position];
				const double componentDegree = componentDegrees[position];
				const double splitGain = 2.0 * resolution * componentDegree *
						(totalCommunityDegree - componentDegree) / (totalWeight * totalWeight);
				const size_t target = component == coreComponent || splitGain < refinementThreshold ? coreComponent : component;
				for (const size_t node: components[component]) refined[node] = target;
			}
		}
		community.swap(refined);
	}

	double LogicalGraphAlgorithms::leidenModularity(const LogicalGraphProjection &projection,
			const std::vector<LogicalGdsNodeCommunity> &communities, const double resolution) {
		if (!std::isfinite(resolution) || resolution <= 0.0) {
			throw std::invalid_argument("logical Leiden modularity requires a positive finite resolution");
		}
		const ValidatedCsr view = requireWellFormedCsr(projection, {});
		const std::shared_ptr<const LogicalProjectionCsr> &csr = view.csr;
		std::unordered_map<uint64_t, uint64_t> communityByNode;
		communityByNode.reserve(communities.size());
		for (const LogicalGdsNodeCommunity &community: communities) {
			if (community.node_id == 0U || community.community_id == 0U ||
				!communityByNode.emplace(community.node_id, community.community_id).second) {
				throw std::invalid_argument("logical Leiden modularity received invalid community assignments");
			}
		}
		if (communityByNode.size() != csr->node_ids.size()) {
			throw std::invalid_argument("logical Leiden modularity must assign every projection Node exactly once");
		}
		for (const uint64_t nodeId: csr->node_ids) {
			if (!communityByNode.contains(nodeId)) {
				throw std::invalid_argument("logical Leiden modularity must assign every projection Node exactly once");
			}
		}
		std::unordered_map<uint64_t, double> degreeByCommunity;
		double internalWeight = 0.0;
		double totalWeight = 0.0;
		for (size_t source = 0U; source < csr->node_ids.size(); ++source) {
			const uint64_t sourceCommunity = communityByNode.at(csr->node_ids[source]);
			for (size_t edge = csr->offsets[source]; edge < csr->offsets[source + 1U]; ++edge) {
				// Leiden is defined over a weakly-undirected relationship view. Do
				// not count the projection builder's synthetic reverse arc twice.
				if (csr->synthetic_reverse[edge] != uint8_t{0U}) {
					continue;
				}
				// requireWellFormedCsr() and the complete assignment check above
				// establish the validity of this edge and both community lookups.
				const double weight = csr->weights[edge];
				const uint64_t targetCommunity = communityByNode.at(csr->target_node_ids[edge]);
				degreeByCommunity[sourceCommunity] += weight;
				degreeByCommunity[targetCommunity] += weight;
				totalWeight += 2.0 * weight;
				if (sourceCommunity == targetCommunity)
					internalWeight += 2.0 * weight;
			}
		}
		if (!std::isfinite(totalWeight)) {
			throw std::runtime_error("logical Leiden modularity total relationship weight overflowed");
		}
		if (totalWeight == 0.0) return 0.0;
		double modularity = internalWeight / totalWeight;
		for (const auto &[ignored, degree]: degreeByCommunity) {
			static_cast<void>(ignored);
			modularity -= resolution * (degree / totalWeight) * (degree / totalWeight);
		}
		return modularity;
	}

	LogicalGdsWeightedPath LogicalGraphAlgorithms::dijkstra(const LogicalGraphProjection &projection,
			const uint64_t sourceNodeId, const uint64_t targetNodeId, const LogicalGraphAlgorithmBounds bounds,
			const LogicalGraphAlgorithmCheckpoint &check) {
		const ValidatedCsr view = requireValidCsr(projection, bounds,
				"logical Dijkstra exceeded its materialized state byte bound", check);
		const auto source = view.node_to_index.find(sourceNodeId);
		const auto target = view.node_to_index.find(targetNodeId);
		if (source == view.node_to_index.end() || target == view.node_to_index.end()) {
			return {};
		}
		if (view.csr->edgeCount() > bounds.maximum_work_units) {
			throw std::runtime_error("logical Dijkstra exceeds its configured work bound");
		}
		const size_t nodeCount = view.csr->node_ids.size();
		const size_t baseStateBytes = checkedAdd(view.node_to_index_bytes,
				checkedMultiply(nodeCount, sizeof(double) + sizeof(size_t) + sizeof(double),
						std::numeric_limits<size_t>::max()), std::numeric_limits<size_t>::max());
		if (bounds.maximum_materialized_bytes != 0U && baseStateBytes > bounds.maximum_materialized_bytes) {
			throw std::runtime_error("logical Dijkstra exceeded its materialized state byte bound");
		}
		const double infinity = std::numeric_limits<double>::infinity();
		std::vector<double> distance(nodeCount, infinity);
		std::vector<size_t> parent(nodeCount, nodeCount);
		std::vector<double> parentWeight(nodeCount, 0.0);
		using QueueEntry = std::pair<double, size_t>;
		std::vector<QueueEntry> frontier;
		size_t admittedFrontierCapacity = 0U;
		const auto admitFrontierCapacity = [&](const size_t required) {
			if (required <= frontier.capacity()) return;
			const size_t targetCapacity = std::max(required,
					frontier.capacity() == 0U ? 1U
											: checkedMultiply(frontier.capacity(), 2U, std::numeric_limits<size_t>::max()));
			if (bounds.maximum_materialized_bytes != 0U) {
				const size_t queueBytes = checkedMultiply(targetCapacity, sizeof(QueueEntry),
						std::numeric_limits<size_t>::max());
				if (queueBytes > bounds.maximum_materialized_bytes - baseStateBytes) {
					throw std::runtime_error("logical Dijkstra exceeded its materialized state byte bound");
				}
			}
			frontier.reserve(targetCapacity);
			admittedFrontierCapacity = frontier.capacity();
			if (bounds.maximum_materialized_bytes != 0U &&
					checkedMultiply(admittedFrontierCapacity, sizeof(QueueEntry), std::numeric_limits<size_t>::max()) >
						bounds.maximum_materialized_bytes - baseStateBytes) {
				throw std::runtime_error("logical Dijkstra exceeded its materialized state byte bound");
			}
		};
		const auto admitResultCapacity = [&](const size_t required, const size_t currentCapacity) {
			if (required <= currentCapacity || bounds.maximum_materialized_bytes == 0U) return;
			const size_t targetCapacity = std::max(required,
					currentCapacity == 0U ? 1U
										: checkedMultiply(currentCapacity, 2U, std::numeric_limits<size_t>::max()));
			const size_t queueBytes = checkedMultiply(admittedFrontierCapacity, sizeof(QueueEntry),
					std::numeric_limits<size_t>::max());
			const size_t resultBytes = checkedMultiply(targetCapacity, sizeof(uint64_t) + sizeof(double),
					std::numeric_limits<size_t>::max());
			if (queueBytes > bounds.maximum_materialized_bytes - baseStateBytes ||
					resultBytes > bounds.maximum_materialized_bytes - baseStateBytes - queueBytes) {
				throw std::runtime_error("logical Dijkstra exceeded its materialized result byte bound");
			}
		};
		distance[source->second] = 0.0;
		admitFrontierCapacity(1U);
		frontier.push_back({0.0, source->second});
		std::push_heap(frontier.begin(), frontier.end(), std::greater<>{});
		size_t usedWork = 0U;
		while (!frontier.empty()) {
			checkpoint(check);
			std::pop_heap(frontier.begin(), frontier.end(), std::greater<>{});
			const auto [currentDistance, current] = frontier.back();
			frontier.pop_back();
			if (currentDistance != distance[current]) {
				continue;
			}
			if (current == target->second) {
				break;
			}
			for (size_t edge = view.csr->offsets[current]; edge < view.csr->offsets[current + 1U]; ++edge) {
				const size_t next = view.node_to_index.at(view.csr->target_node_ids[edge]);
				const double candidate = currentDistance + view.csr->weights[edge];
				if (!std::isfinite(candidate)) {
					throw std::runtime_error("logical Dijkstra distance overflowed");
				}
				// Keep the first shortest predecessor admitted by the canonical CSR
				// traversal. Rewriting equal-distance parents can create a parent
				// cycle through a zero-weight cycle, making result reconstruction
				// non-terminating.
				if (candidate < distance[next]) {
					distance[next] = candidate;
					parent[next] = current;
					parentWeight[next] = view.csr->weights[edge];
					admitFrontierCapacity(checkedAdd(frontier.size(), 1U,
							std::numeric_limits<size_t>::max()));
					frontier.push_back({candidate, next});
					std::push_heap(frontier.begin(), frontier.end(), std::greater<>{});
				}
				consumeWork(usedWork, 1U, bounds);
			}
		}
		if (!std::isfinite(distance[target->second])) {
			return {};
		}
		LogicalGdsWeightedPath result;
		result.total_weight = distance[target->second];
		size_t resultCapacity = 0U;
		for (size_t current = target->second;; current = parent[current]) {
			const size_t nextSize = checkedAdd(result.node_ids.size(), 1U, bounds.maximum_nodes);
			if (nextSize > resultCapacity) {
				const size_t nextCapacity = std::max(nextSize,
						resultCapacity == 0U ? 1U
										: checkedMultiply(resultCapacity, 2U, std::numeric_limits<size_t>::max()));
				admitResultCapacity(nextSize, resultCapacity);
				result.node_ids.reserve(nextCapacity);
				result.step_weights.reserve(nextCapacity);
				resultCapacity = std::min(result.node_ids.capacity(), result.step_weights.capacity());
				if (bounds.maximum_materialized_bytes != 0U) {
					const size_t queueBytes = checkedMultiply(admittedFrontierCapacity, sizeof(QueueEntry),
							std::numeric_limits<size_t>::max());
					const size_t actualResultBytes = checkedAdd(
							checkedMultiply(result.node_ids.capacity(), sizeof(uint64_t),
									std::numeric_limits<size_t>::max()),
							checkedMultiply(result.step_weights.capacity(), sizeof(double),
									std::numeric_limits<size_t>::max()),
							std::numeric_limits<size_t>::max());
					if (queueBytes > bounds.maximum_materialized_bytes - baseStateBytes ||
							actualResultBytes > bounds.maximum_materialized_bytes - baseStateBytes - queueBytes) {
						throw std::runtime_error("logical Dijkstra exceeded its materialized result byte bound");
					}
				}
			}
			result.node_ids.push_back(view.csr->node_ids[current]);
			result.step_weights.push_back(current == source->second ? 0.0 : parentWeight[current]);
			if (current == source->second) {
				break;
			}
			if (parent[current] == nodeCount) {
				throw std::logic_error("logical Dijkstra lost a reachable parent");
			}
		}
		std::ranges::reverse(result.node_ids);
		std::ranges::reverse(result.step_weights);
		return result;
	}

} // namespace graph::query::gds
