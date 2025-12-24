/**
 * @file ProjectOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"

namespace graph::query::execution::operators {

	class ProjectOperator : public PhysicalOperator {
	public:
		/**
		 * @brief Constructs a ProjectOperator.
		 * @param child Upstream operator.
		 * @param variableNames List of variable names to keep in the result.
		 */
		ProjectOperator(std::unique_ptr<PhysicalOperator> child, std::vector<std::string> variableNames) :
			child_(std::move(child)), variableNames_(std::move(variableNames)) {}

		void open() override {
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
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

		void close() override {
			if (child_)
				child_->close();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return variableNames_; }

		// --- Visualization ---
		[[nodiscard]] std::string toString() const override {
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

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_)
				return {child_.get()};
			return {};
		}

	private:
		std::unique_ptr<PhysicalOperator> child_;
		std::vector<std::string> variableNames_;
	};

} // namespace graph::query::execution::operators
