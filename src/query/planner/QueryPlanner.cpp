/**
 * @file QueryPlanner.cpp
 * @author Nexepic
 * @date 2025/3/21
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

#include "graph/query/planner/QueryPlanner.hpp"

#include "graph/query/execution/operators/AlgoShortestPathOperator.hpp"
#include "graph/query/execution/operators/CartesianProductOperator.hpp"
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/CreateNodeOperator.hpp"
#include "graph/query/execution/operators/CreateVectorIndexOperator.hpp"
#include "graph/query/execution/operators/DeleteOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/LimitOperator.hpp"
#include "graph/query/execution/operators/MergeNodeOperator.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/operators/OptionalMatchOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/execution/operators/ShowIndexesOperator.hpp"
#include "graph/query/execution/operators/SingleRowOperator.hpp"
#include "graph/query/execution/operators/SkipOperator.hpp"
#include "graph/query/execution/operators/TraversalOperator.hpp"
#include "graph/query/execution/operators/UnwindOperator.hpp"
#include "graph/query/execution/operators/VarLengthTraversalOperator.hpp"
#include "graph/query/optimizer/Optimizer.hpp"
#include "graph/query/planner/ProcedureRegistry.hpp"

namespace graph::query {

	QueryPlanner::QueryPlanner(std::shared_ptr<storage::DataManager> dm,
							   const std::shared_ptr<indexes::IndexManager> &im) : dm_(std::move(dm)), im_(im) {

		// Initialize the Optimizer
		optimizer_ = std::make_unique<optimizer::Optimizer>(im_);
	}

	QueryPlanner::~QueryPlanner() = default;

	// --- Read Operations ---

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::scanOp(const std::string &variable,
																	  const std::string &label, const std::string &key,
																	  const PropertyValue &value) const {

		// 1. Consult the Optimizer
		// The optimizer decides if we can use an IndexScan or must fallback to FullScan.
		auto config = optimizer_->optimizeNodeScan(variable, label, key, value);

		// 2. Build the Scan Operator
		// The Scan Operator purely executes the config (fetching IDs).
		std::unique_ptr<execution::PhysicalOperator> rootOp =
				std::make_unique<execution::operators::NodeScanOperator>(dm_, im_, config);

		// 3. Handle Residual Predicates (The "Purity" Logic)
		// We requested a scan with a property filter (key, value).
		// - If Optimizer chose PROPERTY_SCAN: The Scan result is already filtered by the Index.
		// - If Optimizer chose LABEL/FULL_SCAN: The Scan result contains ALL nodes.
		//   We MUST add a FilterOperator to check the property.

		if (!key.empty()) {
			if (config.type != execution::ScanType::PROPERTY_SCAN) {
				// Case: Index NOT used. Add explicit Filter.
				auto predicate = [variable, key, value](const execution::Record &r) {
					auto n = r.getNode(variable);
					if (!n)
						return false;
					const auto &props = n->getProperties();
					auto it = props.find(key);
					return it != props.end() && it->second == value;
				};

				std::string desc = variable + "." + key + " == " + value.toString() + " (Residual)";
				rootOp = filterOp(std::move(rootOp), predicate, desc);
			} else {
				// Case: Index USED.
				// Technically, we could skip the filter.
				// However, for 100% robustness against Hash Collisions in the index,
				// some engines add a "Verify" filter here too.
				// For your current stage, we can trust the index and skip the extra filter.
			}
		}

		return rootOp;
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::filterOp(std::unique_ptr<execution::PhysicalOperator> child,
						   std::function<bool(const execution::Record &)> predicate, const std::string &description) {
		return std::make_unique<execution::operators::FilterOperator>(std::move(child), std::move(predicate),
																	  description);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::traverseOp(std::unique_ptr<execution::PhysicalOperator> source, const std::string &sourceVar,
							 const std::string &edgeVar, const std::string &targetVar, const std::string &edgeLabel,
							 const std::string &direction) const {
		return std::make_unique<execution::operators::TraversalOperator>(dm_, std::move(source), sourceVar, edgeVar,
																		 targetVar, edgeLabel, direction);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::projectOp(std::unique_ptr<execution::PhysicalOperator> child,
							const std::vector<execution::operators::ProjectItem> &items,
							bool distinct) {
		return std::make_unique<execution::operators::ProjectOperator>(std::move(child), items, distinct);
	}

	// --- Write Operations ---

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::createOp(const std::string &variable, const std::string &label,
						   const std::unordered_map<std::string, PropertyValue> &props) const {
		return std::make_unique<execution::operators::CreateNodeOperator>(dm_, variable, label, props);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::createOp(const std::string &variable, const std::string &label,
						   const std::unordered_map<std::string, PropertyValue> &props, const std::string &sourceVar,
						   const std::string &targetVar) const {
		return std::make_unique<execution::operators::CreateEdgeOperator>(dm_, variable, label, props, sourceVar,
																		  targetVar);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::callProcedureOp(const std::string &procedure, const std::vector<PropertyValue> &args) const {

		// 1. Prepare Context
		const planner::ProcedureContext ctx{dm_, im_};

		// 2. Lookup and Execute Factory
		if (const auto factory = planner::ProcedureRegistry::instance().get(procedure)) {
			return factory(ctx, args);
		}

		throw std::runtime_error("Unknown procedure: " + procedure);
	}

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::createIndexOp(const std::string &indexName,
																			 const std::string &label,
																			 const std::string &propertyKey) const {
		return std::make_unique<execution::operators::CreateIndexOperator>(im_, indexName, label, propertyKey);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::mergeOp(const std::string &variable, const std::string &label,
						  const std::unordered_map<std::string, PropertyValue> &matchProps,
						  const std::vector<execution::operators::SetItem> &onCreateItems,
						  const std::vector<execution::operators::SetItem> &onMatchItems) const {
		return std::make_unique<execution::operators::MergeNodeOperator>(dm_, im_, variable, label, matchProps,
																		 onCreateItems, onMatchItems);
	}

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::showIndexesOp() const {
		return std::make_unique<execution::operators::ShowIndexesOperator>(im_);
	}

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::dropIndexOp(const std::string &name) const {
		return std::make_unique<execution::operators::DropIndexOperator>(im_, name);
	}

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::dropIndexOp(const std::string &label,
																		   const std::string &propertyKey) const {
		return std::make_unique<execution::operators::DropIndexOperator>(im_, label, propertyKey);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::deleteOp(std::unique_ptr<execution::PhysicalOperator> child,
						   const std::vector<std::string> &variables, bool detach) const {
		return std::make_unique<execution::operators::DeleteOperator>(dm_, std::move(child), variables, detach);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::setOp(std::unique_ptr<execution::PhysicalOperator> child,
						const std::vector<execution::operators::SetItem> &items) const {
		return std::make_unique<execution::operators::SetOperator>(dm_, std::move(child), items);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::removeOp(std::unique_ptr<execution::PhysicalOperator> child,
						   const std::vector<execution::operators::RemoveItem> &items) const {
		return std::make_unique<execution::operators::RemoveOperator>(dm_, std::move(child), items);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::limitOp(std::unique_ptr<execution::PhysicalOperator> child, int64_t limit) {
		return std::make_unique<execution::operators::LimitOperator>(std::move(child), limit);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::skipOp(std::unique_ptr<execution::PhysicalOperator> child, int64_t offset) {
		return std::make_unique<execution::operators::SkipOperator>(std::move(child), offset);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::sortOp(std::unique_ptr<execution::PhysicalOperator> child,
						 const std::vector<execution::operators::SortItem> &items) {
		return std::make_unique<execution::operators::SortOperator>(std::move(child), items);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::traverseVarLengthOp(std::unique_ptr<execution::PhysicalOperator> source, const std::string &sourceVar,
									  const std::string &targetVar, const std::string &edgeLabel, int minHops,
									  int maxHops, const std::string &direction) const {
		return std::make_unique<execution::operators::VarLengthTraversalOperator>(
				dm_, std::move(source), sourceVar, targetVar, edgeLabel, minHops, maxHops, direction);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::cartesianProductOp(std::unique_ptr<execution::PhysicalOperator> left,
									 std::unique_ptr<execution::PhysicalOperator> right) {
		return std::make_unique<execution::operators::CartesianProductOperator>(std::move(left), std::move(right));
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::optionalMatchOp(std::unique_ptr<execution::PhysicalOperator> input,
								 std::unique_ptr<execution::PhysicalOperator> optionalPattern,
								 const std::vector<std::string> &requiredVariables) {
		return std::make_unique<execution::operators::OptionalMatchOperator>(
				std::move(input), std::move(optionalPattern), requiredVariables);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::unwindOp(std::unique_ptr<execution::PhysicalOperator> child, const std::string &alias,
						   const std::vector<PropertyValue> &list) {
		return std::make_unique<execution::operators::UnwindOperator>(std::move(child), alias, list);
	}

	std::unique_ptr<execution::PhysicalOperator>
	QueryPlanner::createVectorIndexOp(const std::string &indexName, const std::string &label,
									  const std::string &property, int dimension, const std::string &metric) const {
		return std::make_unique<execution::operators::CreateVectorIndexOperator>(im_, indexName, label, property,
																				 dimension, metric);
	}

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::singleRowOp() {
		return std::make_unique<execution::operators::SingleRowOperator>();
	}

} // namespace graph::query
