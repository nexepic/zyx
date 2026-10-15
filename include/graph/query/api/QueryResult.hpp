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

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "ResultValue.hpp"
#include "graph/query/api/ResultStorageBytes.hpp"
#include "graph/query/execution/ResourceCoordinator.hpp"

namespace graph::query {

	namespace execution { class LogicalPlanReadRuntime; }

	class QueryResult {
	public:
		QueryResult() = default;
		explicit QueryResult(execution::detail::ResourceLease charge)
			: resource_charge_(prepareEmptyCharge(std::move(charge))) {}
		QueryResult(const QueryResult &other)
			: resource_charge_(other.resource_charge_ ? std::optional(other.resource_charge_->duplicate()) : std::nullopt),
			  rows_(other.rows_), columns_(other.columns_), duration_ms_(other.duration_ms_),
			  notifications_(other.notifications_), pipeline_row_count_(other.pipeline_row_count_) {}
		QueryResult(QueryResult &&) noexcept = default;
		QueryResult &operator=(const QueryResult &other) {
			if (this != &other) { QueryResult copy(other); swap(copy); }
			return *this;
		}
		QueryResult &operator=(QueryResult &&other) noexcept {
			if (this != &other) { QueryResult moved(std::move(other)); swap(moved); }
			return *this;
		}

		using Row = std::unordered_map<std::string, ResultValue>;

		// Main data ingestion point
		void addRow(Row row) {
			if (!resource_charge_) { rows_.push_back(std::move(row)); return; }
			const size_t bytes = detail::resultRowHeapBytes(row);
			appendCharged(rows_, std::move(row), bytes);
		}
		void reserveRows(size_t capacity) { reserveCharged(rows_, capacity); }

		void setColumns(const std::vector<std::string> &cols) {
            if (&cols == &columns_) return;
            if (!resource_charge_) { std::vector<std::string> copy(cols); columns_.swap(copy); return; }
            const auto previous = resource_charge_->bytes();
            const auto oldBytes = detail::resultStringsHeapBytes(columns_);
            const auto upper = detail::resultStringsCopyHeapUpper(cols);
            resource_charge_->set(execution::detail::addPropertyBufferBytes(previous, upper));
            try {
                { std::vector<std::string> copy(cols); columns_.swap(copy); }
            } catch (...) { resource_charge_->set(previous); throw; }
            resource_charge_->set(previous - oldBytes + detail::resultStringsHeapBytes(columns_));
        }

        void setColumns(std::vector<std::string> &&cols) {
			if (!resource_charge_) { columns_ = std::move(cols); return; }
			const size_t oldBytes = detail::resultStringsHeapBytes(columns_);
			const size_t newBytes = detail::resultStringsHeapBytes(cols);
			const size_t previous = resource_charge_->bytes();
			resource_charge_->set(execution::detail::addPropertyBufferBytes(previous, newBytes));
			columns_ = std::move(cols);
			resource_charge_->set(previous - oldBytes + newBytes);
		}

		[[nodiscard]] const std::vector<std::string> &getColumns() const { return columns_; }

		[[nodiscard]] const std::vector<Row> &getRows() const { return rows_; }
		[[nodiscard]] size_t rowCount() const { return rows_.size(); }

		[[nodiscard]] bool isEmpty() const { return rows_.empty(); }

		// Mutation runtimes may intentionally suppress entity columns that no
		// longer exist after the statement while an outer aggregate still needs
		// the cardinality of the rows that flowed through the write pipeline.
		void setPipelineRowCount(size_t count) { pipeline_row_count_ = count; }
		[[nodiscard]] size_t getPipelineRowCount() const { return pipeline_row_count_.value_or(rows_.size()); }

		void setDuration(double ms) { duration_ms_ = ms; }
		[[nodiscard]] double getDuration() const { return duration_ms_; }

		void addNotification(std::string msg) {
			if (!resource_charge_) { notifications_.push_back(std::move(msg)); return; }
			const size_t bytes = execution::detail::dynamicStringBufferBytes(msg);
			appendCharged(notifications_, std::move(msg), bytes);
		}
		[[nodiscard]] const std::vector<std::string> &getNotifications() const { return notifications_; }

		[[nodiscard]] size_t retainedResourceBytes() const noexcept { return resource_charge_ ? resource_charge_->bytes() : 0U; }

	private:
		static execution::detail::ResourceLease prepareEmptyCharge(execution::detail::ResourceLease charge) {
			if (charge.bytes() != 0U) throw std::invalid_argument("new query result requires an empty resource lease");
			// Admit debug iterator proxies before member containers are constructed.
			charge.set(3U * execution::detail::standardContainerProxyBytes());
			return charge;
		}
		friend class execution::LogicalPlanReadRuntime;
		void retainExecutionCharge(execution::detail::ResourceLease charge) {
			if (resource_charge_) throw std::logic_error("query result already owns a resource lease");
			resource_charge_.emplace(std::move(charge));
		}
		void swap(QueryResult &other) noexcept {
			resource_charge_.swap(other.resource_charge_);
			rows_.swap(other.rows_); columns_.swap(other.columns_);
			std::swap(duration_ms_, other.duration_ms_);
			notifications_.swap(other.notifications_); pipeline_row_count_.swap(other.pipeline_row_count_);
		}
		template<typename Value>
		void reserveCharged(std::vector<Value> &values, size_t capacity) {
			if (!resource_charge_ || capacity <= values.capacity()) { values.reserve(capacity); return; }
			using namespace execution::detail;
			const size_t previous = resource_charge_->bytes();
			const size_t oldBytes = standardVectorHeapBufferBytes(values) - standardContainerProxyBytes();
			resource_charge_->set(addPropertyBufferBytes(previous, standardAllocatorRequestBytes(
				multiplyPropertyBufferBytes(capacity, sizeof(Value)), alignof(Value))));
			try { values.reserve(capacity); }
			catch (...) { resource_charge_->set(previous); throw; }
			resource_charge_->set(addPropertyBufferBytes(previous - oldBytes,
				standardVectorHeapBufferBytes(values) - standardContainerProxyBytes()));
		}
		template<typename Value>
		void appendCharged(std::vector<Value> &values, Value &&value, size_t payloadBytes) {
			using namespace execution::detail;
			const size_t previous = resource_charge_->bytes();
			const size_t oldBufferBytes = standardVectorHeapBufferBytes(values);
			resource_charge_->set(addPropertyBufferBytes(previous, payloadBytes));
			try {
				if (values.size() == values.capacity()) reserveCharged(values,
					values.empty() ? 1U : multiplyPropertyBufferBytes(values.size(), 2U));
				values.push_back(std::move(value));
			} catch (...) {
				resource_charge_->set(addPropertyBufferBytes(previous,
					standardVectorHeapBufferBytes(values) - oldBufferBytes));
				throw;
			}
		}
		// Keep the hierarchy alive until after all retained containers are freed.
		std::optional<execution::detail::ResourceLease> resource_charge_;
		std::vector<Row> rows_;
		std::vector<std::string> columns_;

		double duration_ms_ = 0.0;
		std::vector<std::string> notifications_;
		std::optional<size_t> pipeline_row_count_;
	};

} // namespace graph::query
