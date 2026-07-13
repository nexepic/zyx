/**
 * @file ProcedureRegistry.cpp
 * @author Nexepic
 * @date 2025/12/30
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/query/planner/ProcedureRegistry.hpp"
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include "graph/query/execution/operators/AlgoShortestPathOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/GdsOperators.hpp"
#include "graph/query/execution/operators/ListConfigOperator.hpp"
#include "graph/query/execution/operators/ResetStatsOperator.hpp"
#include "graph/query/execution/operators/SetConfigOperator.hpp"
#include "graph/query/execution/operators/ShowStatsOperator.hpp"
#include "graph/query/execution/operators/TrainVectorIndexOperator.hpp"
#include "graph/query/execution/operators/VectorSearchOperator.hpp"
#include "graph/query/planner/ProjectionSpecParser.hpp"

namespace {

using graph::PropertyType;
using graph::PropertyValue;

struct LeidenProcedureOptions {
	int maxIterations = 20;
	double resolution = 1.0;
	size_t threadCount = 0;
	int maxLevels = 10;
	double refinementThreshold = 0.01;
};

const PropertyValue *findOption(const PropertyValue::MapType &options,
                                std::initializer_list<const char *> keys) {
	for (const char *key : keys) {
		auto it = options.find(key);
		if (it != options.end()) return &it->second;
	}
	return nullptr;
}

int parsePositiveIntOption(const PropertyValue &value, const char *name) {
	if (value.getType() != PropertyType::INTEGER) {
		throw std::runtime_error(std::string("gds.leiden.stream option '") + name + "' must be an integer");
	}
	const int64_t parsed = std::get<int64_t>(value.getVariant());
	if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
		throw std::runtime_error(std::string("gds.leiden.stream option '") + name + "' must be a positive integer");
	}
	return static_cast<int>(parsed);
}

size_t parseNonNegativeSizeOption(const PropertyValue &value, const char *name) {
	if (value.getType() != PropertyType::INTEGER) {
		throw std::runtime_error(std::string("gds.leiden.stream option '") + name + "' must be an integer");
	}
	const int64_t parsed = std::get<int64_t>(value.getVariant());
	if (parsed < 0) {
		throw std::runtime_error(std::string("gds.leiden.stream option '") + name + "' must be a non-negative integer");
	}
	return static_cast<size_t>(parsed);
}

double parseFiniteDoubleOption(const PropertyValue &value, const char *name, bool requirePositive) {
	double parsed = 0.0;
	if (value.getType() == PropertyType::INTEGER) {
		parsed = static_cast<double>(std::get<int64_t>(value.getVariant()));
	} else if (value.getType() == PropertyType::DOUBLE) {
		parsed = std::get<double>(value.getVariant());
	} else {
		throw std::runtime_error(std::string("gds.leiden.stream option '") + name + "' must be numeric");
	}
	if (!std::isfinite(parsed) || (requirePositive && parsed <= 0.0) || (!requirePositive && parsed < 0.0)) {
		throw std::runtime_error(
			std::string("gds.leiden.stream option '") + name +
			(requirePositive ? "' must be a positive finite number" : "' must be a non-negative finite number"));
	}
	return parsed;
}

LeidenProcedureOptions parseLeidenOptions(const std::vector<PropertyValue> &args) {
	if (args.size() < 1) {
		throw std::runtime_error(
			"gds.leiden.stream expects (graphName[, options]) or (graphName[, maxIterations, resolution[, threadCount[, maxLevels[, refinementThreshold]]]])");
	}
	if (args.size() > 6) {
		throw std::runtime_error("gds.leiden.stream received too many arguments");
	}

	LeidenProcedureOptions options;
	if (args.size() > 1 && args[1].getType() == PropertyType::MAP) {
		if (args.size() > 2) {
			throw std::runtime_error("gds.leiden.stream options map must be the final argument");
		}
		const auto &map = args[1].getMap();
		if (const auto *value = findOption(map, {"maxIterations", "max_iterations"}))
			options.maxIterations = parsePositiveIntOption(*value, "maxIterations");
		if (const auto *value = findOption(map, {"resolution"}))
			options.resolution = parseFiniteDoubleOption(*value, "resolution", true);
		if (const auto *value = findOption(map, {"threadCount", "thread_count", "concurrency"}))
			options.threadCount = parseNonNegativeSizeOption(*value, "threadCount");
		if (const auto *value = findOption(map, {"maxLevels", "max_levels"}))
			options.maxLevels = parsePositiveIntOption(*value, "maxLevels");
		if (const auto *value = findOption(map, {"refinementThreshold", "refinement_threshold"}))
			options.refinementThreshold = parseFiniteDoubleOption(*value, "refinementThreshold", false);
		return options;
	}

	if (args.size() > 1) options.maxIterations = parsePositiveIntOption(args[1], "maxIterations");
	if (args.size() > 2) options.resolution = parseFiniteDoubleOption(args[2], "resolution", true);
	if (args.size() > 3) options.threadCount = parseNonNegativeSizeOption(args[3], "threadCount");
	if (args.size() > 4) options.maxLevels = parsePositiveIntOption(args[4], "maxLevels");
	if (args.size() > 5) options.refinementThreshold = parseFiniteDoubleOption(args[5], "refinementThreshold", false);
	return options;
}

} // namespace

namespace graph::query::planner {

	ProcedureRegistry::ProcedureRegistry() {
		// --- DBMS Configuration ---
		registerProcedure("dbms.setConfig", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() != 2)
				throw std::runtime_error("dbms.setConfig expects (key, value)");
			return std::make_unique<execution::operators::SetConfigOperator>(ctx.dataManager, args[0].toString(),
																			 args[1]);
		}, /*mutatesData=*/true);

		registerProcedure("dbms.listConfig", [](const ProcedureContext &ctx, const auto & /*args*/) {
			return std::make_unique<execution::operators::ListConfigOperator>(ctx.dataManager);
		});

		registerProcedure("dbms.getConfig", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() != 1)
				throw std::runtime_error("dbms.getConfig expects (key)");
			return std::make_unique<execution::operators::ListConfigOperator>(ctx.dataManager, args[0].toString());
		});

		// --- Algorithms ---
		registerProcedure("algo.shortestPath", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() < 2)
				throw std::runtime_error("algo.shortestPath expects (startId, endId)");
			int64_t start = 0, end = 0;
			int maxDepth = 15;
			try {
				start = std::stoll(args[0].toString());
				end = std::stoll(args[1].toString());
				if (args.size() > 2) {
					const int64_t parsedMaxDepth = std::stoll(args[2].toString());
					if (parsedMaxDepth <= 0 || parsedMaxDepth > std::numeric_limits<int>::max()) {
						throw std::out_of_range("maxDepth must be a positive int");
					}
					maxDepth = static_cast<int>(parsedMaxDepth);
				}
			} catch (...) {
				throw std::runtime_error("IDs and maxDepth must be numeric and maxDepth must be positive");
			}
			return std::make_unique<execution::operators::AlgoShortestPathOperator>(ctx.dataManager, start, end, "both", maxDepth);
		});

		// --- Index Administration (Now Integrated!) ---

		registerProcedure(
				"dbms.createLabelIndex",
				[](const ProcedureContext &ctx,
				   const std::vector<PropertyValue> & /*args*/) -> std::unique_ptr<execution::PhysicalOperator> {
					return std::make_unique<execution::operators::CreateIndexOperator>(ctx.indexManager,
																					   "node_label_idx", "", "");
				}, /*mutatesData=*/false, /*mutatesSchema=*/true);

		registerProcedure("dbms.dropLabelIndex",
						  [](const ProcedureContext &ctx, const std::vector<PropertyValue> & /*args*/)
								  -> std::unique_ptr<execution::PhysicalOperator> {
							  return std::make_unique<execution::operators::DropIndexOperator>(ctx.indexManager,
																							   "node_label_idx");
						  }, /*mutatesData=*/false, /*mutatesSchema=*/true);

		registerProcedure("db.index.vector.queryNodes", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() != 3) {
				throw std::runtime_error("db.index.vector.queryNodes expects (indexName, topK, queryVector)");
			}

			std::string indexName = args[0].toString();
			int64_t k = std::stoll(args[1].toString());

			std::vector<float> queryVec;

			if (args[2].getType() == PropertyType::LIST) {
				// Convert std::vector<PropertyValue> to std::vector<float> for vector index
				const auto &propVec = args[2].getList();
				queryVec.reserve(propVec.size());
				for (const auto &elem : propVec) {
					if (elem.getType() == PropertyType::DOUBLE) {
						queryVec.push_back(static_cast<float>(std::get<double>(elem.getVariant())));
					} else if (elem.getType() == PropertyType::INTEGER) {
						queryVec.push_back(static_cast<float>(std::get<int64_t>(elem.getVariant())));
					} else {
						throw std::runtime_error("Vector index requires numeric values only");
					}
				}
			} else {
				// Fallback: If for some reason the parser didn't produce a LIST type,
				// check if it's strictly required or handle parsing errors.
				throw std::runtime_error("queryVector argument must be a List of floats.");
			}

			return std::make_unique<execution::operators::VectorSearchOperator>(ctx.dataManager, ctx.indexManager,
																				indexName, k, std::move(queryVec));
		});

		registerProcedure("db.index.vector.train", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() != 1) {
				throw std::runtime_error("Usage: db.index.vector.train('index_name')");
			}
			std::string indexName = args[0].toString();

			return std::make_unique<execution::operators::TrainVectorIndexOperator>(ctx.indexManager, indexName);
		}, /*mutatesData=*/true);

		// --- Statistics ---
		registerProcedure("dbms.showStats", [](const ProcedureContext &ctx, const auto & /*args*/) {
			execution::operators::ShowStatsOperator::CacheStats planStats{ctx.planCacheHits, ctx.planCacheMisses};
			return std::make_unique<execution::operators::ShowStatsOperator>(ctx.dataManager, ctx.indexManager, planStats);
		});

		registerProcedure("dbms.resetStats", [](const ProcedureContext &ctx, const auto & /*args*/) {
			return std::make_unique<execution::operators::ResetStatsOperator>(ctx.dataManager, ctx.indexManager);
		}, /*mutatesData=*/true);

		// --- GDS Graph Projection ---
		registerProcedure("gds.graph.project", [](const ProcedureContext &ctx, const auto &args) {
			auto spec = ProjectionSpecParser::parseGraphProjectArgs(args);
			return std::make_unique<execution::operators::GdsGraphProjectOperator>(
				ctx.dataManager, ctx.projectionManager, std::move(spec));
		}, /*mutatesData=*/false);

		registerProcedure("gds.graph.drop", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() != 1)
				throw std::runtime_error("gds.graph.drop expects (name)");
			return std::make_unique<execution::operators::GdsGraphDropOperator>(
				ctx.projectionManager, args[0].toString());
		}, /*mutatesData=*/false);

		registerProcedure("gds.graph.dropAll", [](const ProcedureContext &ctx, const auto &args) {
			if (!args.empty())
				throw std::runtime_error("gds.graph.dropAll expects no arguments");
			return std::make_unique<execution::operators::GdsGraphDropAllOperator>(ctx.projectionManager);
		}, /*mutatesData=*/false);

		registerProcedure("gds.graph.exists", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() != 1)
				throw std::runtime_error("gds.graph.exists expects (name)");
			return std::make_unique<execution::operators::GdsGraphExistsOperator>(
				ctx.projectionManager, args[0].toString());
		}, /*mutatesData=*/false);

		registerProcedure("gds.graph.list", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() > 1)
				throw std::runtime_error("gds.graph.list expects ([name])");
			std::optional<std::string> name;
			if (!args.empty()) {
				name = args[0].toString();
			}
			return std::make_unique<execution::operators::GdsGraphListOperator>(
				ctx.dataManager, ctx.projectionManager, std::move(name));
		}, /*mutatesData=*/false);

		registerProcedure("gds.graph.schema", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() != 1)
				throw std::runtime_error("gds.graph.schema expects (name)");
			return std::make_unique<execution::operators::GdsGraphSchemaOperator>(
				ctx.dataManager, ctx.projectionManager, args[0].toString());
		}, /*mutatesData=*/false);

		// --- GDS Algorithms ---
		registerProcedure("gds.shortestPath.dijkstra.stream", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() < 3)
				throw std::runtime_error("gds.shortestPath.dijkstra.stream expects (graphName, startId, endId)");
			std::string graphName = args[0].toString();
			int64_t start = std::stoll(args[1].toString());
			int64_t end = std::stoll(args[2].toString());
			return std::make_unique<execution::operators::GdsDijkstraOperator>(
				ctx.dataManager, ctx.projectionManager, graphName, start, end);
		});

		registerProcedure("gds.pageRank.stream", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() < 1)
				throw std::runtime_error("gds.pageRank.stream expects (graphName[, maxIterations, dampingFactor])");
			std::string graphName = args[0].toString();
			int maxIter = args.size() > 1 ? static_cast<int>(std::stoll(args[1].toString())) : 20;
			double damping = args.size() > 2 ? std::stod(args[2].toString()) : 0.85;
			return std::make_unique<execution::operators::GdsPageRankOperator>(
				ctx.dataManager, ctx.projectionManager, graphName, maxIter, damping);
		});

		registerProcedure("gds.wcc.stream", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() < 1)
				throw std::runtime_error("gds.wcc.stream expects (graphName)");
			return std::make_unique<execution::operators::GdsWccOperator>(
				ctx.dataManager, ctx.projectionManager, args[0].toString());
		});

		registerProcedure("gds.betweenness.stream", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() < 1)
				throw std::runtime_error("gds.betweenness.stream expects (graphName[, samplingSize])");
			std::string graphName = args[0].toString();
			int sampling = args.size() > 1 ? static_cast<int>(std::stoll(args[1].toString())) : 0;
			return std::make_unique<execution::operators::GdsBetweennessOperator>(
				ctx.dataManager, ctx.projectionManager, graphName, sampling);
		});

		registerProcedure("gds.closeness.stream", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() < 1)
				throw std::runtime_error("gds.closeness.stream expects (graphName)");
			return std::make_unique<execution::operators::GdsClosenessOperator>(
				ctx.dataManager, ctx.projectionManager, args[0].toString());
		});

		registerProcedure("gds.leiden.stream", [](const ProcedureContext &ctx, const auto &args) {
			const auto options = parseLeidenOptions(args);
			std::string graphName = args[0].toString();
			return std::make_unique<execution::operators::GdsLeidenOperator>(
				ctx.dataManager, ctx.projectionManager, graphName, options.maxIterations,
				options.resolution, options.threadCount, options.maxLevels,
				options.refinementThreshold);
		});
	}
} // namespace graph::query::planner
