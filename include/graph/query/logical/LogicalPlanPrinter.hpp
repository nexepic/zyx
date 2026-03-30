/**
 * @file LogicalPlanPrinter.hpp
 * @brief Pretty-prints a logical plan tree for EXPLAIN output.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <sstream>
#include <string>
#include "LogicalOperator.hpp"

namespace graph::query::logical {

/**
 * @brief Prints a logical plan tree as an indented string.
 */
class LogicalPlanPrinter {
public:
	/**
	 * @brief Returns a human-readable string of the plan tree.
	 */
	static std::string print(const LogicalOperator *root) {
		if (!root) return "(empty plan)";
		std::ostringstream out;
		printNode(root, out, 0);
		return out.str();
	}

private:
	static void printNode(const LogicalOperator *node, std::ostringstream &out, int depth) {
		for (int i = 0; i < depth; ++i) out << "  ";
		out << node->toString() << "\n";
		for (auto *child : node->getChildren()) {
			if (child) printNode(child, out, depth + 1);
		}
	}
};

} // namespace graph::query::logical
