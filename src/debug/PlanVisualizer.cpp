/**
 * @file PlanVisualizer.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "PlanVisualizer.hpp"

namespace graph::debug {

	using namespace query::execution;

	std::string PlanVisualizer::visualize(const PhysicalOperator* root) {
		if (!root) return "Null Plan";

		std::ostringstream oss;
		oss << "\nExecution Plan:\n";
		printRecursive(root, "", true, oss);
		return oss.str();
	}

	void PlanVisualizer::printRecursive(const PhysicalOperator* op,
										std::string prefix,
										bool isLast,
										std::ostringstream& oss) {
		// 1. Print current node
		oss << prefix;

		// Choose connector style
		if (isLast) {
			oss << "└── ";
			prefix += "    ";
		} else {
			oss << "├── ";
			prefix += "│   ";
		}

		// Print operator description
		oss << op->toString() << "\n";

		// 2. Process children
		auto children = op->getChildren();
		for (size_t i = 0; i < children.size(); ++i) {
			bool lastChild = (i == children.size() - 1);
			printRecursive(children[i], prefix, lastChild, oss);
		}
	}

} // namespace graph::debug