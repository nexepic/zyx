/**
 * @file leiden_bench.cpp
 * @brief Storage-neutral Leiden benchmark over a synthetic logical snapshot.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#elif defined(_WIN32)
#include <psapi.h>
#include <windows.h>
#endif

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/gds/LogicalGraphAlgorithms.hpp"
#include "graph/query/gds/LogicalGraphProjection.hpp"

namespace {

	using Clock = std::chrono::steady_clock;
	using graph::query::gds::LogicalGraphProjection;
	using graph::query::gds::LogicalGraphProjectionSpec;
	using graph::query::gds::LogicalLeidenOptions;
	using graph::storage::LogicalGraphAdjacencyContinuation;
	using graph::storage::LogicalGraphAdjacencyPage;
	using graph::storage::LogicalGraphEdge;
	using graph::storage::LogicalGraphMutationFence;
	using graph::storage::LogicalGraphNode;
	using graph::storage::LogicalGraphNodePage;
	using graph::storage::LogicalGraphReadDirection;
	using graph::storage::LogicalGraphReadSnapshot;

	struct Args {
		std::string scale = "small";
		std::string graph_type = "clique-chain";
		size_t thread_count = 0U;
		bool help = false;
	};

	struct GraphHandle {
		size_t nodes = 0U;
		size_t edges = 0U;
		size_t maximum_outgoing_edges = 0U;
	};

	class SyntheticSnapshot final : public LogicalGraphReadSnapshot {
	public:
		std::vector<LogicalGraphNode> nodes;
		std::vector<LogicalGraphEdge> edges;
		std::vector<std::vector<size_t>> outgoing_edge_indices;
		std::vector<std::vector<size_t>> incoming_edge_indices;

		[[nodiscard]] LogicalGraphNode readNode(const uint64_t node_id) const override {
			const auto found = std::ranges::find(nodes, node_id, &LogicalGraphNode::node_id);
			if (found == nodes.end()) {
				throw std::out_of_range("unknown synthetic Node");
			}
			return *found;
		}

		[[nodiscard]] bool containsNode(const uint64_t node_id) const override {
			return std::ranges::find(nodes, node_id, &LogicalGraphNode::node_id) != nodes.end();
		}

		[[nodiscard]] LogicalGraphEdge readEdge(const uint64_t edge_id) const override {
			const auto found = std::ranges::find(edges, edge_id, &LogicalGraphEdge::edge_id);
			if (found == edges.end()) {
				throw std::out_of_range("unknown synthetic relationship");
			}
			return *found;
		}

		void visitNodes(const size_t maximum_records,
						const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
			if (nodes.size() > maximum_records) {
				throw std::runtime_error("synthetic Node scan exceeded its caller bound");
			}
			for (const LogicalGraphNode &node: nodes) {
				if (!visitor(node)) {
					return;
				}
			}
		}

		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(const uint64_t after_node_id, const size_t maximum_records,
														  const size_t maximum_metadata_pages) const override {
			if (maximum_records == 0U || maximum_metadata_pages == 0U) {
				throw std::invalid_argument("synthetic Node scan requires positive bounds");
			}
			LogicalGraphNodePage page;
			for (const LogicalGraphNode &node: nodes) {
				if (node.node_id <= after_node_id) {
					continue;
				}
				if (page.nodes.size() == maximum_records) {
					page.may_have_more = true;
					break;
				}
				page.nodes.push_back(node);
			}
			page.metadata_pages_visited = page.nodes.empty() ? 0U : 1U;
			return page;
		}

		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(const uint64_t node_id, const LogicalGraphReadDirection direction,
							  const std::optional<LogicalGraphAdjacencyContinuation> continuation,
							  const size_t maximum_records) const override {
			if (node_id == 0U || node_id > outgoing_edge_indices.size() || maximum_records == 0U) {
				throw std::invalid_argument("synthetic adjacency scan requires positive valid bounds");
			}
			const std::vector<size_t> &indices = adjacentEdgeIndices(node_id, direction);
			size_t first = 0U;
			if (continuation.has_value()) {
				if (continuation->previous_edge_id == 0U || continuation->next_edge_id == 0U) {
					throw std::invalid_argument("synthetic adjacency continuation has an invalid Edge ID");
				}
				const auto next = std::ranges::find(indices, continuation->next_edge_id,
															[&](const size_t index) { return edges[index].edge_id; });
				if (next == indices.end() || next == indices.begin() ||
					edges[*std::prev(next)].edge_id != continuation->previous_edge_id) {
					throw std::runtime_error("synthetic adjacency continuation does not match the logical Edge order");
				}
				first = static_cast<size_t>(std::distance(indices.begin(), next));
			}
			LogicalGraphAdjacencyPage page;
			page.edges.reserve(std::min(maximum_records, indices.size() - first));
			for (size_t index = first; index < indices.size() && page.edges.size() < maximum_records; ++index) {
				page.edges.push_back(edges[indices[index]]);
			}
			const size_t next = first + page.edges.size();
			if (next < indices.size()) {
				page.continuation = {page.edges.back().edge_id, edges[indices[next]].edge_id};
			}
			return page;
		}

		void visitEdges(const size_t maximum_records,
						const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			if (edges.size() > maximum_records) {
				throw std::runtime_error("synthetic relationship scan exceeded its caller bound");
			}
			for (const LogicalGraphEdge &edge: edges) {
				if (!visitor(edge)) {
					return;
				}
			}
		}

		void visitAdjacentEdges(const uint64_t node_id, const LogicalGraphReadDirection direction,
								const size_t maximum_edges,
								const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			if (node_id == 0U || node_id > outgoing_edge_indices.size() || maximum_edges == 0U || !visitor) {
				throw std::invalid_argument("synthetic adjacency visit requires positive valid bounds and a visitor");
			}
			size_t visited = 0U;
			const std::vector<size_t> &indices = adjacentEdgeIndices(node_id, direction);
			for (const size_t edge_index: indices) {
				if (visited == maximum_edges) {
					throw std::runtime_error("synthetic adjacency scan exceeded its caller bound");
				}
				++visited;
				if (!visitor(edges.at(edge_index))) {
					return;
				}
			}
		}

		[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
			return LogicalGraphMutationFence{.backend_token = 1U, .format_token = 1U};
		}

	private:
		[[nodiscard]] const std::vector<size_t> &adjacentEdgeIndices(const uint64_t node_id,
															const LogicalGraphReadDirection direction) const {
			const size_t index = static_cast<size_t>(node_id - 1U);
			switch (direction) {
				case LogicalGraphReadDirection::LGRD_OUTGOING:
					return outgoing_edge_indices.at(index);
				case LogicalGraphReadDirection::LGRD_INCOMING:
					return incoming_edge_indices.at(index);
			}
			throw std::invalid_argument("synthetic adjacency scan has an unknown direction");
		}
	};

	void printHelp() {
		std::cerr << "Usage: leiden-bench [options]\n"
				  << "  --scale <s>        smoke|small|medium|large (default small)\n"
				  << "  --graph <g>        clique-chain|random (default clique-chain)\n"
				  << "  --threads <n>      worker threads; 0 auto-detects (default)\n"
				  << "  -h, --help         show this help\n";
	}

	[[nodiscard]] Args parseArgs(const int argc, char **argv) {
		Args arguments;
		for (int index = 1; index < argc; ++index) {
			const std::string argument = argv[index];
			const auto next = [&]() -> std::string {
				if (index + 1 >= argc) {
					throw std::invalid_argument("missing value for " + argument);
				}
				return argv[++index];
			};
			if (argument == "-h" || argument == "--help") {
				arguments.help = true;
			} else if (argument == "--scale") {
				arguments.scale = next();
			} else if (argument == "--graph") {
				arguments.graph_type = next();
			} else if (argument == "--threads") {
				arguments.thread_count = static_cast<size_t>(std::stoull(next()));
			} else {
				throw std::invalid_argument("unknown argument: " + argument);
			}
		}
		return arguments;
	}

	[[nodiscard]] size_t nodeCountForScale(const std::string &scale) {
		if (scale == "smoke")
			return 1'000U;
		if (scale == "small")
			return 10'000U;
		if (scale == "medium")
			return 50'000U;
		if (scale == "large")
			return 100'000U;
		throw std::invalid_argument("unknown scale: " + scale + " (smoke|small|medium|large)");
	}

	[[nodiscard]] size_t peakRssKiB() {
#if defined(__APPLE__) || defined(__linux__)
		rusage usage{};
		if (getrusage(RUSAGE_SELF, &usage) != 0) {
			throw std::runtime_error("getrusage failed while reading peak RSS");
		}
#if defined(__APPLE__)
		return static_cast<size_t>(usage.ru_maxrss) / 1024U;
#else
		return static_cast<size_t>(usage.ru_maxrss);
#endif
#elif defined(_WIN32)
		PROCESS_MEMORY_COUNTERS counters{};
		counters.cb = sizeof(counters);
		if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
			throw std::runtime_error("GetProcessMemoryInfo failed while reading peak RSS");
		}
		return static_cast<size_t>(counters.PeakWorkingSetSize / 1024U);
#else
		return 0U;
#endif
	}

	[[nodiscard]] double secondsSince(const Clock::time_point start) {
		return std::chrono::duration<double>(Clock::now() - start).count();
	}

	void appendEdge(SyntheticSnapshot &snapshot, uint64_t &next_edge_id, const uint64_t source, const uint64_t target,
					std::vector<size_t> &outgoing_counts) {
		const size_t edge_index = snapshot.edges.size();
		snapshot.edges.emplace_back(next_edge_id++, source, target, "E");
		snapshot.outgoing_edge_indices.at(static_cast<size_t>(source - 1U)).push_back(edge_index);
		snapshot.incoming_edge_indices.at(static_cast<size_t>(target - 1U)).push_back(edge_index);
		++outgoing_counts.at(static_cast<size_t>(source - 1U));
	}

	[[nodiscard]] GraphHandle buildCliqueChain(SyntheticSnapshot &snapshot, const size_t node_count) {
		constexpr size_t CLIQUE_SIZE = 10U;
		std::vector<size_t> outgoing_counts(node_count, 0U);
		snapshot.nodes.reserve(node_count);
		snapshot.outgoing_edge_indices.resize(node_count);
		snapshot.incoming_edge_indices.resize(node_count);
		for (size_t index = 0U; index < node_count; ++index) {
			snapshot.nodes.emplace_back(static_cast<uint64_t>(index + 1U), std::vector<std::string>{"N"});
		}
		uint64_t next_edge_id = 1U;
		uint64_t previous_last = 0U;
		for (size_t group_begin = 0U; group_begin < node_count; group_begin += CLIQUE_SIZE) {
			const size_t group_end = std::min(group_begin + CLIQUE_SIZE, node_count);
			for (size_t source = group_begin; source < group_end; ++source) {
				for (size_t target = source + 1U; target < group_end; ++target) {
					appendEdge(snapshot, next_edge_id, source + 1U, target + 1U, outgoing_counts);
				}
			}
			if (previous_last != 0U) {
				appendEdge(snapshot, next_edge_id, previous_last, static_cast<uint64_t>(group_begin + 1U),
						   outgoing_counts);
			}
			previous_last = static_cast<uint64_t>(group_end);
		}
		return {.nodes = node_count,
				.edges = snapshot.edges.size(),
				.maximum_outgoing_edges = *std::ranges::max_element(outgoing_counts)};
	}

	[[nodiscard]] GraphHandle buildRandomGraph(SyntheticSnapshot &snapshot, const size_t node_count) {
		std::vector<size_t> outgoing_counts(node_count, 0U);
		snapshot.nodes.reserve(node_count);
		snapshot.outgoing_edge_indices.resize(node_count);
		snapshot.incoming_edge_indices.resize(node_count);
		for (size_t index = 0U; index < node_count; ++index) {
			snapshot.nodes.emplace_back(static_cast<uint64_t>(index + 1U), std::vector<std::string>{"N"});
		}
		if (node_count < 2U) {
			return {.nodes = node_count, .edges = 0U, .maximum_outgoing_edges = 0U};
		}
		std::mt19937_64 random(42U);
		const double probability = 6.0 / static_cast<double>(node_count - 1U);
		std::geometric_distribution<size_t> gaps(probability);
		const size_t pair_count = node_count * (node_count - 1U) / 2U;
		size_t pair_index = gaps(random);
		uint64_t next_edge_id = 1U;
		while (pair_index < pair_count) {
			size_t low = 0U;
			size_t high = node_count - 1U;
			while (low < high) {
				const size_t middle = low + (high - low) / 2U;
				const size_t first_pair = middle * (2U * (node_count - 1U) - middle + 1U) / 2U;
				if (first_pair <= pair_index) {
					low = middle + 1U;
				} else {
					high = middle;
				}
			}
			const size_t source = low == 0U ? 0U : low - 1U;
			const size_t first_pair = source * (2U * (node_count - 1U) - source + 1U) / 2U;
			const size_t target = source + 1U + pair_index - first_pair;
			if (target < node_count) {
				appendEdge(snapshot, next_edge_id, source + 1U, target + 1U, outgoing_counts);
			}
			const size_t gap = gaps(random);
			if (gap > std::numeric_limits<size_t>::max() - pair_index - 1U) {
				break;
			}
			pair_index += gap + 1U;
		}
		return {.nodes = node_count,
				.edges = snapshot.edges.size(),
				.maximum_outgoing_edges = *std::ranges::max_element(outgoing_counts)};
	}

	[[nodiscard]] size_t deriveWorkBudget(const size_t node_count, const size_t edge_count,
										  const LogicalLeidenOptions &options) {
		constexpr size_t FIXED_EDGE_PHASES = 8U;
		if (options.maximum_iterations > std::numeric_limits<size_t>::max() - FIXED_EDGE_PHASES) {
			throw std::overflow_error("Leiden benchmark work budget overflowed");
		}
		const size_t edge_phase_count = options.maximum_iterations + FIXED_EDGE_PHASES;
		if (edge_count > std::numeric_limits<size_t>::max() / edge_phase_count) {
			throw std::overflow_error("Leiden benchmark work budget overflowed");
		}
		const size_t per_level_edges = edge_count * edge_phase_count;
		if (node_count > std::numeric_limits<size_t>::max() - per_level_edges) {
			throw std::overflow_error("Leiden benchmark work budget overflowed");
		}
		const size_t per_level = std::max<size_t>(1U, node_count + per_level_edges);
		if (options.maximum_levels > std::numeric_limits<size_t>::max() / per_level) {
			throw std::overflow_error("Leiden benchmark work budget overflowed");
		}
		return per_level * options.maximum_levels;
	}

} // namespace

int main(const int argc, char **argv) {
	try {
		const Args args = parseArgs(argc, argv);
		if (args.help) {
			printHelp();
			return 0;
		}
		const size_t node_count = nodeCountForScale(args.scale);
		SyntheticSnapshot snapshot;

		const Clock::time_point build_start = Clock::now();
		const GraphHandle graph = args.graph_type == "clique-chain" ? buildCliqueChain(snapshot, node_count)
								  : args.graph_type == "random"
										  ? buildRandomGraph(snapshot, node_count)
										  : throw std::invalid_argument("unknown graph type (clique-chain|random)");
		const double build_seconds = secondsSince(build_start);

		const Clock::time_point projection_start = Clock::now();
		LogicalGraphProjectionSpec spec;
		spec.name = "leiden-benchmark";
		const LogicalGraphProjection projection = LogicalGraphProjection::build(
				snapshot, spec,
				{.maximum_nodes = graph.nodes,
				 .maximum_metadata_pages = 1U,
				 .maximum_edges_per_node = std::max<size_t>(1U, graph.maximum_outgoing_edges)});
		const double projection_seconds = secondsSince(projection_start);

		const LogicalLeidenOptions options;
		const graph::query::gds::LogicalGraphAlgorithmBounds bounds{
				.maximum_nodes = graph.nodes,
				.maximum_edges = projection.edgeCount(),
				.maximum_iterations = options.maximum_iterations,
				.maximum_work_units = deriveWorkBudget(graph.nodes, projection.edgeCount(), options),
		};
		graph::concurrent::ThreadPool pool(args.thread_count);
		const size_t rss_before_leiden = peakRssKiB();

		const Clock::time_point leiden_start = Clock::now();
		const std::vector<graph::query::gds::LogicalGdsNodeCommunity> communities =
				graph::query::gds::LogicalGraphAlgorithms::leiden(projection, options, bounds, {}, &pool);
		const double leiden_seconds = secondsSince(leiden_start);
		const double modularity = graph::query::gds::LogicalGraphAlgorithms::leidenModularity(projection, communities);
		std::set<uint64_t> community_ids;
		for (const auto &community: communities) {
			community_ids.insert(community.community_id);
		}
		const size_t peak_rss = peakRssKiB();

		std::cout << "leiden-bench scale=" << args.scale << " graph=" << args.graph_type << " nodes=" << graph.nodes
				  << " relationships=" << graph.edges << " threads=" << pool.getThreadCount() << '\n';
		std::cout << "result phase=build seconds=" << build_seconds << "\n";
		std::cout << "result phase=projection seconds=" << projection_seconds << " csr_nodes=" << projection.nodeCount()
				  << " csr_arcs=" << projection.edgeCount() << "\n";
		std::cout << "result phase=leiden seconds=" << leiden_seconds << " communities=" << community_ids.size()
				  << " modularity=" << modularity << "\n";
		std::cout << "result peak_rss_kib=" << peak_rss
				  << " leiden_delta_rss_kib=" << (peak_rss > rss_before_leiden ? peak_rss - rss_before_leiden : 0U)
				  << "\n";
		std::cout << "result total_seconds=" << build_seconds + projection_seconds + leiden_seconds << "\n";
		return 0;
	} catch (const std::exception &exception) {
		std::cerr << "error: " << exception.what() << '\n';
		printHelp();
		return 2;
	}
}
