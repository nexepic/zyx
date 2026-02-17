/**
 * @file OperatorChain.cpp
 * @author Nexepic
 * @date 2025/12/9
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

#include "../OperatorChain.hpp"
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/CreateNodeOperator.hpp"

namespace graph::parser::cypher::helpers {

std::unique_ptr<query::execution::PhysicalOperator> OperatorChain::chain(
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	std::unique_ptr<query::execution::PhysicalOperator> newOp) {

	if (!rootOp) {
		// No existing root - newOp becomes the root
		return newOp;
	}

	// Case 1: Write Operators (Pipe) -> Wrap the current root
	// These operators need to process records from the child operator
	if (auto *edgeOp = dynamic_cast<query::execution::operators::CreateEdgeOperator *>(newOp.get())) {
		edgeOp->setChild(std::move(rootOp));
		return newOp;
	}

	if (auto *nodeOp = dynamic_cast<query::execution::operators::CreateNodeOperator *>(newOp.get())) {
		nodeOp->setChild(std::move(rootOp));
		return newOp;
	}

	// Case 2: Other operators replace the current root
	// This handles operators that don't wrap the pipeline
	return newOp;
}

} // namespace graph::parser::cypher::helpers
