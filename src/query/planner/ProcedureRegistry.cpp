/**
 * @file ProcedureRegistry.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/30
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/planner/ProcedureRegistry.hpp"
#include "graph/query/execution/operators/AlgoShortestPathOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/ListConfigOperator.hpp"
#include "graph/query/execution/operators/SetConfigOperator.hpp"

namespace graph::query::planner {

	ProcedureRegistry::ProcedureRegistry() {
		// --- DBMS Configuration ---
		registerProcedure("dbms.setConfig", [](const ProcedureContext &ctx, const auto &args) {
			if (args.size() != 2)
				throw std::runtime_error("dbms.setConfig expects (key, value)");
			return std::make_unique<execution::operators::SetConfigOperator>(ctx.dataManager, args[0].toString(),
																			 args[1]);
		});

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
			try {
				start = std::stoll(args[0].toString());
				end = std::stoll(args[1].toString());
			} catch (...) {
				throw std::runtime_error("IDs must be numeric");
			}
			return std::make_unique<execution::operators::AlgoShortestPathOperator>(ctx.dataManager, start, end);
		});

		// --- Index Administration (Now Integrated!) ---

		registerProcedure(
				"dbms.createLabelIndex",
				[](const ProcedureContext &ctx,
				   const std::vector<PropertyValue> & /*args*/) -> std::unique_ptr<execution::PhysicalOperator> {
					return std::make_unique<execution::operators::CreateIndexOperator>(ctx.indexManager,
																					   "node_label_idx", "", "");
				});

		registerProcedure("dbms.dropLabelIndex",
						  [](const ProcedureContext &ctx, const std::vector<PropertyValue> & /*args*/)
								  -> std::unique_ptr<execution::PhysicalOperator> {
							  return std::make_unique<execution::operators::DropIndexOperator>(ctx.indexManager,
																							   "node_label_idx");
						  });
	}
} // namespace graph::query::planner
