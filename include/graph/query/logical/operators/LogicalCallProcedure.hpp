/**
 * @file LogicalCallProcedure.hpp
 * @brief Logical operator for CALL procedure statements.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once
#include "graph/core/OwnedPropertyVector.hpp"
#include "graph/utils/OwnedStringVector.hpp"
#include "graph/utils/OwnedStdString.hpp"

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/core/PropertyTypes.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalCallProcedure
 * @brief Logical operator that invokes a named stored procedure with arguments.
 *
 * Leaf operator: produces no bindable output variables and has no child.
 */
class LogicalCallProcedure : public LogicalOperator {
public:
	class ArgumentView {
	public:
		[[nodiscard]] size_t size() const { return parameter_names_ == nullptr ? args_->size() : parameter_names_->size(); }
		[[nodiscard]] bool empty() const { return size() == 0U; }
		[[nodiscard]] const graph::PropertyValue &operator[](const size_t index) const {
			if (parameter_names_ == nullptr) return args_->at(index);
			const auto found = parameters_->find(parameter_names_->at(index));
			if (found == parameters_->end()) throw std::logic_error("validated procedure argument disappeared");
			return found->second;
		}
		[[nodiscard]] const graph::PropertyValue &front() const { return (*this)[0U]; }

	private:
		friend class LogicalCallProcedure;
		ArgumentView(const std::vector<graph::PropertyValue> &args, const std::vector<std::string> &parameterNames,
				const std::unordered_map<std::string, graph::PropertyValue> &parameters)
			: args_(&args), parameter_names_(parameterNames.empty() ? nullptr : &parameterNames), parameters_(&parameters) {}
		const std::vector<graph::PropertyValue> *args_;
		const std::vector<std::string> *parameter_names_;
		const std::unordered_map<std::string, graph::PropertyValue> *parameters_;
	};

	LogicalCallProcedure(utils::OwnedStdString procedureName, graph::OwnedPropertyVector args)
		: procedureName_(std::move(procedureName)), args_(std::move(args)) {}

	/**
	 * @brief Creates a procedure call whose arguments are resolved from the
	 * execution context instead of embedded in the logical plan.
	 */
	[[nodiscard]] static std::unique_ptr<LogicalCallProcedure>
	withBoundArguments(utils::OwnedStdString procedureName, utils::OwnedStringVector parameterNames) {
		auto result = std::make_unique<LogicalCallProcedure>(std::move(procedureName),
															std::vector<graph::PropertyValue>{});
		result->parameter_names_ = std::move(parameterNames);
		return result;
	}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_CALL_PROCEDURE; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

    [[nodiscard]] utils::OwnedStringVector getOwnedOutputVariables() const override { return {}; }

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		if (!parameter_names_.empty()) {
			return withBoundArguments(procedureName_, parameter_names_);
		}
		return std::make_unique<LogicalCallProcedure>(procedureName_, args_);
	}

	[[nodiscard]] std::string toString() const override {
		return "CallProcedure(" + procedureName_ + ")";
	}

	void setChild(size_t, std::unique_ptr<LogicalOperator>) override {
		throw std::logic_error("Leaf node");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t) override {
		throw std::logic_error("Leaf node");
	}

	[[nodiscard]] const std::string &getProcedureName() const { return procedureName_; }
	[[nodiscard]] const std::vector<graph::PropertyValue> &getArgs() const { return args_; }
	[[nodiscard]] const std::vector<std::string> &getParameterNames() const { return parameter_names_; }
	[[nodiscard]] bool hasBoundArguments() const { return !parameter_names_.empty(); }

	[[nodiscard]] ArgumentView resolveArgumentView(
			const std::unordered_map<std::string, graph::PropertyValue> &parameters) const {
		for (const std::string &parameterName: parameter_names_) {
			if (parameterName.empty()) throw std::logic_error("logical procedure call has an empty parameter name");
			if (!parameters.contains(parameterName)) {
				throw std::runtime_error("logical procedure call could not resolve a bound argument");
			}
		}
		return ArgumentView(args_, parameter_names_, parameters);
	}

	[[nodiscard]] std::vector<graph::PropertyValue>
	resolveArguments(const std::unordered_map<std::string, graph::PropertyValue> &parameters) const {
		if (parameter_names_.empty()) return args_;
		std::vector<graph::PropertyValue> result;
		result.reserve(parameter_names_.size());
		for (const std::string &parameterName: parameter_names_) {
			if (parameterName.empty()) throw std::logic_error("logical procedure call has an empty parameter name");
			const auto found = parameters.find(parameterName);
			if (found == parameters.end()) {
				throw std::runtime_error("logical procedure call could not resolve a bound argument");
			}
			result.push_back(found->second);
		}
		return result;
	}

private:
	utils::OwnedStdString procedureName_;
	graph::OwnedPropertyVector args_;
	utils::OwnedStringVector parameter_names_;
};

} // namespace graph::query::logical
