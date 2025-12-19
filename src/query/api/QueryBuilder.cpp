/**
 * @file QueryBuilder.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/api/QueryBuilder.hpp"
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/CreateNodeOperator.hpp"

namespace graph::query {

	QueryBuilder::QueryBuilder(std::shared_ptr<QueryPlanner> planner) : planner_(std::move(planner)) {}

	// --- Chain Helper ---
	void QueryBuilder::append(std::unique_ptr<execution::PhysicalOperator> op) {
		if (!root_) {
			root_ = std::move(op);
			return;
		}

		// Try to link 'Create' operators to form a chain (Pipe)
		if (auto *edgeOp = dynamic_cast<execution::operators::CreateEdgeOperator *>(op.get())) {
			edgeOp->setChild(std::move(root_));
			root_ = std::move(op);
			return;
		}
		if (auto *nodeOp = dynamic_cast<execution::operators::CreateNodeOperator *>(op.get())) {
			nodeOp->setChild(std::move(root_));
			root_ = std::move(op);
			return;
		}

		// Default: If the new operator wraps the old root (like Filter/Project/Delete/Set),
		// it should have been constructed that way by the Caller via Planner.
		// However, QueryBuilder calls Planner factories which usually take 'child' as arg.
		// Wait! My QueryPlanner factories (setOp, deleteOp) TAKE the child.
		// So 'op' already contains 'root_'. We just need to update root_.
		root_ = std::move(op);
	}

	// --- Read ---

	QueryBuilder &QueryBuilder::match_(const std::string &variable, const std::string &label, const std::string &key,
									   const PropertyValue &value) {
		// If root exists, this implies a Join/Cartesian (not supported in simple builder yet).
		// For now, we assume MATCH starts the query or resets it.
		root_ = planner_->scanOp(variable, label, key, value);
		return *this;
	}

	QueryBuilder &QueryBuilder::where_(const std::string &variable, const std::string &key,
									   const PropertyValue &value) {
		if (!root_)
			throw std::runtime_error("WHERE called without a preceding MATCH/SCAN");

		auto predicate = [variable, key, value](const execution::Record &r) {
			auto node = r.getNode(variable);
			if (!node)
				return false;
			const auto &props = node->getProperties();
			auto it = props.find(key);
			return it != props.end() && it->second == value;
		};

		std::string desc = variable + "." + key + " == " + value.toString();
		// Wraps current root
		root_ = planner_->filterOp(std::move(root_), predicate, desc);
		return *this;
	}

	// --- Write (Data) ---

	QueryBuilder &QueryBuilder::create_(const std::string &variable, const std::string &label,
										const std::unordered_map<std::string, PropertyValue> &props) {
		auto op = planner_->createOp(variable, label, props);
		append(std::move(op));
		return *this;
	}

	QueryBuilder &QueryBuilder::create_(const std::string &variable, const std::string &label,
										const std::string &sourceVar, const std::string &targetVar,
										const std::unordered_map<std::string, PropertyValue> &props) {
		auto op = planner_->createOp(variable, label, props, sourceVar, targetVar);
		append(std::move(op));
		return *this;
	}

	QueryBuilder &QueryBuilder::delete_(const std::vector<std::string> &variables, bool detach) {
		if (!root_)
			throw std::runtime_error("DELETE must follow a MATCH");
		// Wraps current root
		root_ = planner_->deleteOp(std::move(root_), variables, detach);
		return *this;
	}

	QueryBuilder &QueryBuilder::set_(const std::string &variable, const std::string &key, const PropertyValue &value) {
		if (!root_)
			throw std::runtime_error("SET must follow a MATCH");

		std::vector<execution::operators::SetItem> items;
		items.push_back({variable, key, value});

		// Wraps current root
		root_ = planner_->setOp(std::move(root_), items);
		return *this;
	}

	// --- Write (Index) ---

	QueryBuilder &QueryBuilder::createIndex_(const std::string &label, const std::string &property,
											 const std::string &indexName) {
		auto op = planner_->createIndexOp(indexName, label, property);
		append(std::move(op));
		return *this;
	}

	QueryBuilder &QueryBuilder::dropIndex_(const std::string &indexName) {
		auto op = planner_->dropIndexOp(indexName);
		append(std::move(op));
		return *this;
	}

	QueryBuilder &QueryBuilder::dropIndex_(const std::string &label, const std::string &property) {
		auto op = planner_->dropIndexOp(label, property);
		append(std::move(op));
		return *this;
	}

	QueryBuilder &QueryBuilder::showIndexes_() {
		auto op = planner_->showIndexesOp();
		append(std::move(op));
		return *this;
	}

	// --- Procedures ---

	QueryBuilder &QueryBuilder::call_(const std::string &procedure, const std::vector<PropertyValue> &args) {
		auto op = planner_->callProcedureOp(procedure, args);
		append(std::move(op));
		return *this;
	}

	// --- Finalize ---

	QueryBuilder &QueryBuilder::return_(const std::vector<std::string> &variables) {
		if (!root_)
			return *this;
		root_ = planner_->projectOp(std::move(root_), variables);
		return *this;
	}

	std::unique_ptr<execution::PhysicalOperator> QueryBuilder::build() { return std::move(root_); }

} // namespace graph::query
