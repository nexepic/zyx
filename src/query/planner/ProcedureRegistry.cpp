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
#include "graph/query/execution/operators/AlgoShortestPathOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/ListConfigOperator.hpp"
#include "graph/query/execution/operators/SetConfigOperator.hpp"
#include "graph/query/execution/operators/TrainVectorIndexOperator.hpp"
#include "graph/query/execution/operators/VectorSearchOperator.hpp"

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
		});
	}
} // namespace graph::query::planner
