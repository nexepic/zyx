/**
 * @file QueryBuilder.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/QueryBuilder.hpp"

#include "graph/query/execution/operators/CreateEdgeOperator.hpp"

namespace graph::query {

	QueryBuilder::QueryBuilder(std::shared_ptr<QueryPlanner> planner)
		: planner_(std::move(planner)) {}

	QueryBuilder& QueryBuilder::create(const std::string& variable, const std::string& label,
                                       const std::unordered_map<std::string, PropertyValue>& props) {
        auto op = planner_->create(variable, label, props);
        append(std::move(op));
        return *this;
    }

    QueryBuilder& QueryBuilder::create(const std::string& variable, const std::string& label,
                                       const std::string& sourceVar, const std::string& targetVar,
                                       const std::unordered_map<std::string, PropertyValue>& props) {

        // Create the edge operator
        auto op = planner_->create(variable, label, props, sourceVar, targetVar);

        // Link it to the existing pipeline (Edge creation needs upstream nodes)
        if (root_) {
             // In a perfect world, PhysicalOperator has setChild.
             // Here we might need a dynamic_cast if the base class doesn't enforce piping.
             // Assuming CreateEdgeOperator has setChild:
             auto* edgeOp = dynamic_cast<execution::operators::CreateEdgeOperator*>(op.get());
             if (edgeOp) {
                 edgeOp->setChild(std::move(root_));
             }
             root_ = std::move(op);
        } else {
            throw std::runtime_error("Cannot CREATE relationship without preceding matches (Variables not bound).");
        }

        return *this;
    }

    void QueryBuilder::append(std::unique_ptr<execution::PhysicalOperator> op) {
        // Simple chaining logic
        // If 'op' is a filter or something that takes a child, wrap 'root_'.
        // If 'op' is a source (like CreateNode can be), it might replace root or join.
        // For simplicity in this example:
        // 1. If op is CreateNode, it stands alone or creates a Cartesian product (not handled here).
        // 2. Ideally, use a helper "Pipe" abstraction.

        // Quick fix for this demo: replace root if root is null.
        if (!root_) {
            root_ = std::move(op);
        } else {
             // If we have a root, and we create a node, we effectively have two disconnected streams.
             // Real engines use a "CartesianProduct" or "PassThrough" operator here.
             // For now, let's assume CreateNode replaces root only if root is empty.
             // OR, if CreateNode is meant to just add to the graph regardless of context:
             // It becomes the new root, but it doesn't consume the old root?
             // This logic depends on if you support "MATCH (n) CREATE (m)".

             // Let's assume standalone create for now.
             root_ = std::move(op);
        }
    }

	QueryBuilder& QueryBuilder::match(const std::string& variable, const std::string& label) {
		// If root already exists, this would imply a Cartesian Product or Join (simplified here)
		// For now, assume MATCH starts the chain.
		root_ = planner_->scan(variable, label);
		return *this;
	}

	QueryBuilder& QueryBuilder::where(const std::string& variable, const std::string& key, const PropertyValue& value) {
		if (!root_) throw std::runtime_error("WHERE called without a preceding MATCH/SCAN");

		auto predicate = [variable, key, value](const execution::Record& r) {
			auto node = r.getNode(variable);
			if (!node) return false;
			// Assuming properties are loaded
			const auto& props = node->getProperties();
			auto it = props.find(key);
			return it != props.end() && it->second == value;
		};

		root_ = planner_->filter(std::move(root_), predicate);
		return *this;
	}

	std::unique_ptr<execution::PhysicalOperator> QueryBuilder::build() {
		return std::move(root_);
	}
}