/**
 * @file ProjectOperator.hpp
 * @author Nexepic
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"

namespace graph::query::execution::operators {

	/**
	 * @struct ProjectItem
	 * @brief Defines a single item in a RETURN clause.
	 */
	struct ProjectItem {
		std::shared_ptr<graph::query::expressions::Expression> expression; // Expression AST
		std::string alias; // The output variable name (e.g., "age" or "num")

		// Helper function to check if a character is a valid identifier start
		static bool isIdentifierStart(char c) {
			return std::isalpha(c) || c == '_';
		}

		// Helper function to check if a character is a valid identifier character
		static bool isIdentifierChar(char c) {
			return std::isalnum(c) || c == '_';
		}

		// Helper function to parse simple string expressions into AST
		static std::shared_ptr<graph::query::expressions::Expression> parseExpression(const std::string& exprText) {
			using namespace graph::query::expressions;

			// Check if it's a quoted string literal (e.g., "hello" or 'world')
			if ((exprText.size() >= 2) && ((exprText.front() == '"' && exprText.back() == '"') ||
			                               (exprText.front() == '\'' && exprText.back() == '\''))) {
				std::string strVal = exprText.substr(1, exprText.size() - 2);
				return std::make_shared<LiteralExpression>(strVal);
			}

			// Check if it's a property access (e.g., n.prop)
			// Only treat as property access if it starts with a valid identifier
			size_t dotPos = exprText.find('.');
			if (dotPos != std::string::npos && dotPos > 0 && isIdentifierStart(exprText[0])) {
				std::string varName = exprText.substr(0, dotPos);
				std::string propName = exprText.substr(dotPos + 1);
				// Verify the rest of the variable name is valid
				bool validVarName = true;
				for (char c : varName) {
					if (!isIdentifierChar(c)) {
						validVarName = false;
						break;
					}
				}
				if (validVarName && !propName.empty()) {
					return std::make_shared<VariableReferenceExpression>(varName, propName);
				}
			}

			// Check for NULL keyword (case-insensitive)
			if (exprText.size() == 4) {
				std::string upper = exprText;
				for (auto& c : upper) c = std::toupper(c);
				if (upper == "NULL") {
					return std::make_shared<LiteralExpression>();
				}
			}

			// Check if it's a boolean literal (case-insensitive)
			if (exprText.size() <= 5) {
				std::string upper = exprText;
				for (auto& c : upper) c = std::toupper(c);
				if (upper == "TRUE") {
					return std::make_shared<LiteralExpression>(true);
				}
				if (upper == "FALSE") {
					return std::make_shared<LiteralExpression>(false);
				}
			}

			// Check if it's an integer literal
			try {
				size_t pos;
				int64_t intVal = std::stoll(exprText, &pos);
				if (pos == exprText.length()) {
					return std::make_shared<LiteralExpression>(intVal);
				}
			} catch (...) {
				// Not an integer, continue
			}

			// Check if it's a double literal
			try {
				size_t pos;
				double doubleVal = std::stod(exprText, &pos);
				if (pos == exprText.length()) {
					return std::make_shared<LiteralExpression>(doubleVal);
				}
			} catch (...) {
				// Not a double, continue
			}

			// Default: treat as variable reference
			return std::make_shared<VariableReferenceExpression>(exprText);
		}

		// Legacy constructor for backward compatibility
		ProjectItem(std::string exprText, std::string alias)
			: expression(parseExpression(exprText)), alias(std::move(alias)) {}

		// New constructor for expression AST
		ProjectItem(std::shared_ptr<graph::query::expressions::Expression> expr, std::string alias)
			: expression(std::move(expr)), alias(std::move(alias)) {}

		// Default constructor
		ProjectItem() = default;
	};

	class ProjectOperator : public PhysicalOperator {
	public:
		/**
		 * @brief Constructs a ProjectOperator.
		 * @param child Upstream operator.
		 * @param items List of projection items (expression + alias).
		 * @param distinct If true, eliminate duplicate rows.
		 */
		ProjectOperator(std::unique_ptr<PhysicalOperator> child, std::vector<ProjectItem> items, bool distinct = false) :
			child_(std::move(child)), items_(std::move(items)), distinct_(distinct) {}

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
					std::string key = item.alias;

					// Check if this is a simple variable reference (no property access)
					using namespace graph::query::expressions;
					if (item.expression && item.expression->getExpressionType() == ExpressionType::PROPERTY_ACCESS) {
						auto* varRef = static_cast<VariableReferenceExpression*>(item.expression.get());
						if (!varRef->hasProperty()) {
							// Simple variable reference - try to copy node/edge directly
							const std::string& varName = varRef->getVariableName();

							// Check if it's a node
							if (auto node = record.getNode(varName)) {
								newRecord.setNode(key, *node);
								continue;
							}

							// Check if it's an edge
							if (auto edge = record.getEdge(varName)) {
								newRecord.setEdge(key, *edge);
								continue;
							}

							// Check if it's a value
							if (auto value = record.getValue(varName)) {
								newRecord.setValue(key, *value);
								continue;
							}
						}
					}

					// Evaluate the expression to get the value
					PropertyValue value;
					if (item.expression) {
						try {
							value = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
							    item.expression.get(), record);
						} catch (const graph::query::expressions::UndefinedVariableException&) {
							// Undefined variable → NULL (Cypher semantics)
							value = PropertyValue();
						}
					} else {
						// Fallback to NULL for expressions not yet migrated
						value = PropertyValue();
					}

					// Store the value in the new record
					newRecord.setValue(key, value);
				}

				// DISTINCT: Check if we've seen this record before
				if (distinct_) {
					std::string recordKey = recordToString(newRecord);
					if (seenRecords_.find(recordKey) == seenRecords_.end()) {
						seenRecords_.insert(recordKey);
						outputBatch.push_back(std::move(newRecord));
					}
					// If duplicate, skip adding to outputBatch
				} else {
					outputBatch.push_back(std::move(newRecord));
				}
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
			if (distinct_) oss << "DISTINCT ";
			for (size_t i = 0; i < items_.size(); ++i) {
				if (items_[i].expression) {
					oss << items_[i].expression->toString();
				}
				if (items_[i].alias != (items_[i].expression ? items_[i].expression->toString() : "")) {
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
		bool distinct_;
		std::unordered_set<std::string> seenRecords_; // Track seen records for DISTINCT

		/**
		 * @brief Generate a hash string for a record to use in DISTINCT deduplication.
		 */
		std::string recordToString(const Record &record) {
			std::string result;
			for (const auto &item: items_) {
				if (auto val = record.getValue(item.alias)) {
					result += val->toString() + "|";
				} else {
					result += "NULL|";
				}
			}
			return result;
		}
	};

} // namespace graph::query::execution::operators
