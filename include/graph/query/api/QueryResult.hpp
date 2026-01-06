/**
 * @file QueryResult.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
