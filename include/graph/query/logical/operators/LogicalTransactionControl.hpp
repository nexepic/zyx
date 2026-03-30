/**
 * @file LogicalTransactionControl.hpp
 * @brief Logical operator for transaction control statements (BEGIN, COMMIT, ROLLBACK).
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @enum LogicalTxnCommand
 * @brief Enumerates supported transaction control commands.
 *
 * Prefixed with LTXN_ to avoid Windows macro conflicts.
 */
enum class LogicalTxnCommand {
	LTXN_BEGIN,    ///< BEGIN TRANSACTION
	LTXN_COMMIT,   ///< COMMIT
	LTXN_ROLLBACK  ///< ROLLBACK
};

/**
 * @class LogicalTransactionControl
 * @brief Logical operator that issues a transaction control command.
 *
 * Leaf operator: produces no output variables and has no child.
 */
class LogicalTransactionControl : public LogicalOperator {
public:
	explicit LogicalTransactionControl(LogicalTxnCommand command)
		: command_(command) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_TRANSACTION_CONTROL; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalTransactionControl>(command_);
	}

	[[nodiscard]] std::string toString() const override {
		switch (command_) {
			case LogicalTxnCommand::LTXN_BEGIN:    return "TransactionControl(BEGIN)";
			case LogicalTxnCommand::LTXN_COMMIT:   return "TransactionControl(COMMIT)";
			case LogicalTxnCommand::LTXN_ROLLBACK: return "TransactionControl(ROLLBACK)";
			default:                               return "TransactionControl(UNKNOWN)";
		}
	}

	void setChild(size_t, std::unique_ptr<LogicalOperator>) override {
		throw std::logic_error("Leaf node");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t) override {
		throw std::logic_error("Leaf node");
	}

	[[nodiscard]] LogicalTxnCommand getCommand() const { return command_; }

private:
	LogicalTxnCommand command_;
};

} // namespace graph::query::logical
