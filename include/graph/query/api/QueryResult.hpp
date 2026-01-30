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

		[[nodiscard]] const std::vector<std::string> &getColumns() const { return columns_; }

		[[nodiscard]] const std::vector<Row> &getRows() const { return rows_; }
		[[nodiscard]] size_t rowCount() const { return rows_.size(); }

		[[nodiscard]] bool isEmpty() const { return rows_.empty(); }

		void setDuration(double ms) { duration_ms_ = ms; }
		[[nodiscard]] double getDuration() const { return duration_ms_; }

		void addNotification(std::string msg) { notifications_.push_back(std::move(msg)); }
		[[nodiscard]] const std::vector<std::string> &getNotifications() const { return notifications_; }

	private:
		std::vector<Row> rows_;
		std::vector<std::string> columns_;

		double duration_ms_ = 0.0;
		std::vector<std::string> notifications_;
	};

} // namespace graph::query
