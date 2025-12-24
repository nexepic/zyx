/**
 * @file ProjectOperator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/execution/operators/ProjectOperator.hpp"
#include <sstream>

namespace graph::query::execution::operators {

	ProjectOperator::ProjectOperator(std::unique_ptr<PhysicalOperator> child, std::vector<std::string> variableNames) :
		child_(std::move(child)), variableNames_(std::move(variableNames)) {}

	void ProjectOperator::open() {
		if (child_)
			child_->open();
	}

	std::optional<RecordBatch> ProjectOperator::next() {
		auto batchOpt = child_->next();
		if (!batchOpt)
			return std::nullopt;

		RecordBatch &inputBatch = *batchOpt;
		RecordBatch outputBatch;
		outputBatch.reserve(inputBatch.size());

		for (const auto &record: inputBatch) {
			Record newRecord;

			for (const auto &varExpr: variableNames_) {
				// 1. Try Direct Lookup (Node/Edge/Value)
				// e.g. RETURN n
				bool found = false;
				if (auto node = record.getNode(varExpr)) {
					newRecord.setNode(varExpr, *node);
					found = true;
				} else if (auto edge = record.getEdge(varExpr)) {
					newRecord.setEdge(varExpr, *edge);
					found = true;
				} else if (auto val = record.getValue(varExpr)) {
					newRecord.setValue(varExpr, *val);
					found = true;
				}

				// 2. Try Property Access Logic (e.g. "n.age")
				if (!found) {
					size_t dotPos = varExpr.find('.');
					if (dotPos != std::string::npos) {
						std::string varName = varExpr.substr(0, dotPos);
						std::string propKey = varExpr.substr(dotPos + 1);

						// Look for Node
						if (auto node = record.getNode(varName)) {
							// Check if property exists
							// Note: Properties must be hydrated by ScanOperator!
							const auto &props = node->getProperties();
							auto it = props.find(propKey);
							if (it != props.end()) {
								// Store as a Value in the new record
								// Key in new record is "n.age" (preserving the expression name)
								newRecord.setValue(varExpr, it->second);
								found = true;
							} else {
								// Property missing -> Null
								newRecord.setValue(varExpr, PropertyValue());
								found = true;
							}
						}
						// Look for Edge (if needed)
						else if (auto edge = record.getEdge(varName)) {
							const auto &props = edge->getProperties();
							auto it = props.find(propKey);
							if (it != props.end()) {
								newRecord.setValue(varExpr, it->second);
								found = true;
							}
						}
					}
				}

				// If still not found (e.g. literal or unknown var), currently we skip or set null.
				// For robustness, maybe set null?
				if (!found) {
					// newRecord.setValue(varExpr, PropertyValue());
				}
			}

			outputBatch.push_back(std::move(newRecord));
		}

		return outputBatch;
	}

	void ProjectOperator::close() {
		if (child_)
			child_->close();
	}

	std::vector<std::string> ProjectOperator::getOutputVariables() const { return variableNames_; }

	std::string ProjectOperator::toString() const {
		std::ostringstream oss;
		oss << "Project(";
		for (size_t i = 0; i < variableNames_.size(); ++i) {
			oss << variableNames_[i];
			if (i < variableNames_.size() - 1)
				oss << ", ";
		}
		oss << ")";
		return oss.str();
	}

	std::vector<const PhysicalOperator *> ProjectOperator::getChildren() const {
		if (child_)
			return {child_.get()};
		return {};
	}

} // namespace graph::query::execution::operators
