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

		// Default: New operator wraps the old root
		root_ = std::move(op);
	}

	// --- Read ---

	QueryBuilder &QueryBuilder::match_(const std::string &variable, const std::string &label, const std::string &key,
									   const PropertyValue &value) {
		auto newScan = planner_->scanOp(variable, label, key, value);

		if (!root_) {
			root_ = std::move(newScan);
		} else {
			// Combine new scan with existing plan using Cartesian Product
			// This ensures 'root_' contains variables from both the old plan AND the new scan.
			root_ = planner_->cartesianProductOp(std::move(root_), std::move(newScan));
		}
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
		root_ = planner_->deleteOp(std::move(root_), variables, detach);
		return *this;
	}

	QueryBuilder &QueryBuilder::set_(const std::string &variable, const std::string &key, const PropertyValue &value) {
		if (!root_)
			throw std::runtime_error("SET must follow a MATCH");

		std::vector<execution::operators::SetItem> items;
		// Using SetActionType::PROPERTY
		items.push_back({execution::operators::SetActionType::PROPERTY, variable, key, value});

		root_ = planner_->setOp(std::move(root_), items);
		return *this;
	}

	QueryBuilder &QueryBuilder::setLabel_(const std::string &variable, const std::string &label) {
		if (!root_)
			throw std::runtime_error("SET must follow a MATCH");

		std::vector<execution::operators::SetItem> items;
		// Using SetActionType::LABEL
		items.push_back({
				execution::operators::SetActionType::LABEL, variable, label,
				PropertyValue() // Empty val
		});

		root_ = planner_->setOp(std::move(root_), items);
		return *this;
	}

	QueryBuilder &QueryBuilder::remove_(const std::string &variable, const std::string &key, bool isLabel) {
		if (!root_)
			throw std::runtime_error("REMOVE must follow a MATCH");

		std::vector<execution::operators::RemoveItem> items;
		items.push_back({isLabel ? execution::operators::RemoveActionType::LABEL
								 : execution::operators::RemoveActionType::PROPERTY,
						 variable, key});

		root_ = planner_->removeOp(std::move(root_), items);
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

		// Convert string list to ProjectItem list (alias = expression)
		std::vector<execution::operators::ProjectItem> items;
		for (const auto &var: variables) {
			items.push_back({var, var});
		}

		root_ = planner_->projectOp(std::move(root_), items);
		return *this;
	}

	QueryBuilder &QueryBuilder::skip_(int64_t offset) {
		if (!root_)
			return *this;
		root_ = planner_->skipOp(std::move(root_), offset);
		return *this;
	}

	QueryBuilder &QueryBuilder::limit_(int64_t limit) {
		if (!root_)
			return *this;
		root_ = planner_->limitOp(std::move(root_), limit);
		return *this;
	}

	QueryBuilder &QueryBuilder::orderBy_(const std::vector<SortOrder> &items) {
		if (!root_)
			return *this;

		std::vector<execution::operators::SortItem> sortItems;
		for (const auto &item: items) {
			sortItems.push_back({item.variable, item.property, item.ascending});
		}

		root_ = planner_->sortOp(std::move(root_), sortItems);
		return *this;
	}

	QueryBuilder &QueryBuilder::unwind(const std::vector<PropertyValue> &list, const std::string &alias) {
		root_ = planner_->unwindOp(std::move(root_), alias, list);
		return *this;
	}

	std::unique_ptr<execution::PhysicalOperator> QueryBuilder::build() { return std::move(root_); }

} // namespace graph::query
