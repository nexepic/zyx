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
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace graph::storage { class DataManager; }

namespace graph::query::execution::operators {

	/**
	 * @struct ProjectItem
	 * @brief Defines a single item in a RETURN clause.
	 */
	struct ProjectItem {
		std::shared_ptr<graph::query::expressions::Expression> expression; // Expression AST
		std::string alias; // The output variable name (e.g., "age" or "num")

		// Constructor for expression AST
		ProjectItem(std::shared_ptr<graph::query::expressions::Expression> expr, std::string alias)
			: expression(std::move(expr)), alias(std::move(alias)) {}

		// Default constructor
		ProjectItem() = default;
	};

	class ProjectOperator : public PhysicalOperator {
	public:
		static constexpr size_t PARALLEL_PROJECT_THRESHOLD = 4096;

		/**
		 * @brief Constructs a ProjectOperator.
		 * @param child Upstream operator.
		 * @param items List of projection items (expression + alias).
		 * @param distinct If true, eliminate duplicate rows.
		 */
		ProjectOperator(std::unique_ptr<PhysicalOperator> child, std::vector<ProjectItem> items, bool distinct = false,
		                storage::DataManager *dataManager = nullptr) :
			child_(std::move(child)), items_(std::move(items)), distinct_(distinct), dataManager_(dataManager) {}

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			std::vector<std::string> vars;
			for (const auto &item: items_) {
				vars.push_back(item.alias);
			}
			return vars;
		}

		[[nodiscard]] std::string toString() const override;
		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override;

	private:
		std::unique_ptr<PhysicalOperator> child_;
		std::vector<ProjectItem> items_;
		bool distinct_;
		storage::DataManager *dataManager_ = nullptr;

		/**
		 * @brief A record fingerprint for DISTINCT dedup: stores projected values
		 * with hash and equality comparison.
		 */
		struct RecordFingerprint {
			std::vector<PropertyValue> values;

			bool operator==(const RecordFingerprint &other) const {
				return values == other.values;
			}
		};

		struct RecordFingerprintHash {
			size_t operator()(const RecordFingerprint &fp) const {
				size_t seed = fp.values.size();
				PropertyValueHash pvHash;
				for (const auto &v : fp.values) {
					seed ^= pvHash(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
				}
				return seed;
			}
		};

		std::unordered_set<RecordFingerprint, RecordFingerprintHash> seenRecords_;

		/**
		 * @brief Build a fingerprint for a record from its projected values.
		 */
		RecordFingerprint buildFingerprint(const Record &record);

		/**
		 * @brief Project a single input record into an output record.
		 */
		Record projectRecord(const Record &input) const;
	};

} // namespace graph::query::execution::operators
