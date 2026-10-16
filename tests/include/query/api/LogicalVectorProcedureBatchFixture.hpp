#pragma once
/**
 * @file LogicalVectorProcedureBatchFixture.hpp
 * @brief Vector procedure batching and deferred materialization contracts.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include "query/api/LogicalProcedureTestFixture.hpp"

#include "graph/core/Types.hpp"
#include "graph/query/NativeProcedureManifest.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalExplain.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProfile.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::query {
using test::RecordingVectorProcedureBackend;
		class LogicalVectorPropertyBatchTest : public ::testing::Test {
		protected:
			void SetUp() override {
				for (uint64_t id = 1U; id <= 3U; ++id) {
					storage::LogicalGraphNode node;
					node.node_id = id;
					node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id * 10U))},
							{"body", PropertyValue(std::string(4096U, 'x'))}};
					backend->state->nodes.emplace(id, std::move(node));
				}
				backend->state->search_result.hits = {{3U, 0.0F}, {1U, 1.0F}, {2U, 2.0F}};
			}
			[[nodiscard]] std::unique_ptr<logical::LogicalCallProcedure> search(const int64_t topK = 3) const {
				return std::make_unique<logical::LogicalCallProcedure>("db.index.vector.queryNodes",
						std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(topK),
								PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
			}
			[[nodiscard]] QueryResult execute(const logical::LogicalOperator &root) const {
				execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
				return runtime.execute(root, options.bounds, options.maximum_vector_mapping_entries,
						options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
						options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
						options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
						options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
			}
            // Fixed workspace allowance expressed in the active STL's value and
            // container frame sizes. Input cardinality is deliberately much
            // larger than this workspace and the configured ten output slots.
            [[nodiscard]] size_t boundedTopKFixtureBytes() const {
                return backend->state->search_result.hits.capacity() * sizeof(storage::LogicalGraphVectorSearchHit) +
                    8U * sizeof(execution::LogicalPlanReadRow) +
                    16U * sizeof(execution::LogicalPlanReadValue) + 16U * sizeof(PropertyValue);
            }
			std::shared_ptr<RecordingVectorProcedureBackend> backend = std::make_shared<RecordingVectorProcedureBackend>();
			LogicalReadExecutionOptions options;
		};
} // namespace graph::query
