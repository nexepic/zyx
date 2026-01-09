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
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"

namespace graph::query::execution::operators {

	/**
	 * @struct ProjectItem
	 * @brief Defines a single item in a RETURN clause.
	 */
	struct ProjectItem {
		std::string expression; // The source expression (e.g., "n.age" or "1")
		std::string alias;      // The output variable name (e.g., "age" or "num")
	};

	class ProjectOperator : public PhysicalOperator {
	public:
		/**
		 * @brief Constructs a ProjectOperator.
		 * @param child Upstream operator.
		 * @param items List of projection items (expression + alias).
		 */
		ProjectOperator(std::unique_ptr<PhysicalOperator> child, std::vector<ProjectItem> items) :
			child_(std::move(child)), items_(std::move(items)) {}

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

				for (const auto &item: items_) {
					std::string expr = item.expression;
					std::string key = item.alias;
					bool found = false;

					// 1. Try Direct Lookup (Node/Edge/Value)
					// e.g. RETURN n
					if (auto node = record.getNode(expr)) {
						newRecord.setNode(key, *node);
						found = true;
					} else if (auto edge = record.getEdge(expr)) {
						newRecord.setEdge(key, *edge);
						found = true;
					} else if (auto val = record.getValue(expr)) {
						newRecord.setValue(key, *val);
						found = true;
					}

					// 2. Try Property Access Logic (e.g. "n.age")
					if (!found) {
						size_t dotPos = expr.find('.');
						if (dotPos != std::string::npos) {
							std::string varName = expr.substr(0, dotPos);
							std::string propKey = expr.substr(dotPos + 1);

							// Look for Node
							if (auto node = record.getNode(varName)) {
								// Check if property exists
								// Note: Properties must be hydrated by ScanOperator!
								const auto &props = node->getProperties();
								auto it = props.find(propKey);
								if (it != props.end()) {
									// Store as a Value in the new record using the ALIAS as key
									newRecord.setValue(key, it->second);
									found = true;
								} else {
									// Property missing -> Null
									newRecord.setValue(key, PropertyValue());
									found = true;
								}
							}
							// Look for Edge (if needed)
							else if (auto edge = record.getEdge(varName)) {
								const auto &props = edge->getProperties();
								auto it = props.find(propKey);
								if (it != props.end()) {
									newRecord.setValue(key, it->second);
									found = true;
								}
							}
						}
					}

					// 3. Try Literal Parsing (e.g. RETURN 1)
					if (!found) {
						if (auto literalVal = parseLiteral(expr)) {
							newRecord.setValue(key, *literalVal);
							found = true;
						}
					}

					// If still not found (e.g. unknown var), set to Null.
					if (!found) {
						newRecord.setValue(key, PropertyValue());
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

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			std::vector<std::string> vars;
			for (const auto &item: items_) {
				vars.push_back(item.alias);
			}
			return vars;
		}

		// --- Visualization ---
		[[nodiscard]] std::string toString() const override {
			std::ostringstream oss;
			oss << "Project(";
			for (size_t i = 0; i < items_.size(); ++i) {
				oss << items_[i].expression;
				if (items_[i].alias != items_[i].expression) {
					oss << " AS " << items_[i].alias;
				}
				if (i < items_.size() - 1)
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
		std::vector<ProjectItem> items_;

		/**
		 * @brief Attempts to parse a string expression as a literal value.
		 * Supports Integers, Doubles, Booleans, Strings, and Null.
		 */
		std::optional<PropertyValue> parseLiteral(const std::string &txt) {
			// String: '...' or "..."
			if (txt.size() >= 2 &&
				((txt.front() == '\'' && txt.back() == '\'') || (txt.front() == '"' && txt.back() == '"'))) {
				return PropertyValue(txt.substr(1, txt.size() - 2));
			}
			// Boolean
			if (txt == "TRUE" || txt == "true")
				return PropertyValue(true);
			if (txt == "FALSE" || txt == "false")
				return PropertyValue(false);

			// Null
			if (txt == "NULL" || txt == "null")
				return PropertyValue();

			// Number (Integer or Double)
			// Regex for basic number check: optional -, digits, optional .digits
			static const std::regex numberRegex(R"(^-?\d+(\.\d+)?$)");
			if (std::regex_match(txt, numberRegex)) {
				try {
					if (txt.find('.') != std::string::npos) {
						return PropertyValue(std::stod(txt));
					} else {
						return PropertyValue(std::stoll(txt));
					}
				} catch (...) {
					return std::nullopt; // Parse error
				}
			}

			return std::nullopt; // Not a recognized literal
		}
	};

} // namespace graph::query::execution::operators