/**
 * @file LogicalNamedPath.hpp
 * @brief Logical operator for named path binding: p = (a)-[r]->(b)
 */

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalNamedPath
 * @brief Wraps a pattern subtree and binds the path to a variable.
 *
 * Collects the node and edge variables from the pattern and constructs
 * a path LIST value bound to the path variable name.
 */
class LogicalNamedPath : public LogicalOperator {
public:
	LogicalNamedPath(
		std::unique_ptr<LogicalOperator> child,
		std::string pathVariable,
		std::vector<std::string> nodeVariables,
		std::vector<std::string> edgeVariables
	) : child_(std::move(child)),
		pathVariable_(std::move(pathVariable)),
		nodeVariables_(std::move(nodeVariables)),
		edgeVariables_(std::move(edgeVariables)) {}

	[[nodiscard]] LogicalOpType getType() const override {
		return LogicalOpType::LOP_NAMED_PATH;
	}

	[[nodiscard]] std::vector<LogicalOperator*> getChildren() const override {
		return {child_.get()};
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
		vars.push_back(pathVariable_);
		return vars;
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalNamedPath>(
			child_ ? child_->clone() : nullptr,
			pathVariable_, nodeVariables_, edgeVariables_);
	}

	[[nodiscard]] std::string toString() const override {
		return "NamedPath(" + pathVariable_ + ")";
	}

	void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
		if (index == 0) child_ = std::move(child);
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
		if (index == 0) return std::move(child_);
		return nullptr;
	}

	[[nodiscard]] const std::string& getPathVariable() const { return pathVariable_; }
	[[nodiscard]] const std::vector<std::string>& getNodeVariables() const { return nodeVariables_; }
	[[nodiscard]] const std::vector<std::string>& getEdgeVariables() const { return edgeVariables_; }

private:
	std::unique_ptr<LogicalOperator> child_;
	std::string pathVariable_;
	std::vector<std::string> nodeVariables_;
	std::vector<std::string> edgeVariables_;
};

} // namespace graph::query::logical
