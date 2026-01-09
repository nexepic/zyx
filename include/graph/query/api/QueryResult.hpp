/**
 * @file QueryResult.hpp
 * @author Nexepic
 * @date 2025/3/20
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

#include <string>
#include <unordered_map>
#include <vector>
#include "ResultValue.hpp"

namespace graph::query {

	class QueryResult {
	public:
		QueryResult() = default;

		using Row = std::unordered_map<std::string, ResultValue>;

		// Main data ingestion point
		void addRow(Row row) { rows_.push_back(std::move(row)); }

		void setColumns(std::vector<std::string> cols) { columns_ = std::move(cols); }

		const std::vector<std::string> &getColumns() const { return columns_; }

		const std::vector<Row> &getRows() const { return rows_; }
		size_t rowCount() const { return rows_.size(); }

		bool isEmpty() const { return rows_.empty(); }

	private:
		std::vector<Row> rows_;
		std::vector<std::string> columns_;
	};

} // namespace graph::query
