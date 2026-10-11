/**
 * @file zyx.hpp
 * @author Nexepic
 * @date 2025/12/16
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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include "value.hpp"

namespace zyx {

	class Database;
	class Result;
	class PreparedQuery;
	class QueryBuilder;

	namespace detail {
		class DatabaseBulkInternal;
		class PreparedQueryImpl;
		class QueryBuilderImpl;
		QueryBuilderImpl &requireQueryBuilderImpl(QueryBuilder &builder);
		Value getTypedResultValue(const Result &result, int index);
	}

	// Forward declarations of implementation classes.
	// These are defined ONLY in the .cpp file to keep this header clean.
	class DatabaseImpl;
	class ResultImpl;
	class TransactionImpl;

	/**
	 * @brief An immutable, storage-neutral query prepared by QueryBuilder.
	 *
	 * A prepared query stores only a typed logical plan and bound values. It
	 * never contains Cypher text, a planner, storage handles, or physical
	 * operators, so it is safe to reuse across executions of the same Database.
	 */
	class PreparedQuery {
	public:
		~PreparedQuery();
		PreparedQuery(PreparedQuery &&) noexcept;
		PreparedQuery &operator=(PreparedQuery &&) noexcept;
		PreparedQuery(const PreparedQuery &) = delete;
		PreparedQuery &operator=(const PreparedQuery &) = delete;

		[[nodiscard]] bool empty() const noexcept;

	private:
		std::unique_ptr<detail::PreparedQueryImpl> impl_;

		explicit PreparedQuery(std::unique_ptr<detail::PreparedQueryImpl> impl);
		friend class QueryBuilder;
		friend class Database;
		friend class Transaction;
	};

	/**
	 * @brief Fluent builder for the native logical query boundary.
	 *
	 * All identifiers are validated when build() is called. Values are retained
	 * as typed bound values and are never formatted into a Cypher string.
	 */
	class QueryBuilder {
	public:
		struct SortOrder {
			std::string variable;
			std::string property;
			bool ascending = true;
		};

		~QueryBuilder();
		QueryBuilder(QueryBuilder &&) noexcept;
		QueryBuilder &operator=(QueryBuilder &&) noexcept;
		QueryBuilder(const QueryBuilder &) = delete;
		QueryBuilder &operator=(const QueryBuilder &) = delete;

		QueryBuilder &match(const std::string &variable, const std::string &label = "",
							const std::string &key = "", const Value &value = Value{});
		QueryBuilder &match(const std::string &variable, const std::vector<std::string> &labels,
							const std::string &key = "", const Value &value = Value{});
		QueryBuilder &where(const std::string &variable, const std::string &key, const Value &value);

		QueryBuilder &createNode(const std::string &variable, const std::string &label,
								 const std::unordered_map<std::string, Value> &properties = {});
		QueryBuilder &createNode(const std::string &variable, const std::vector<std::string> &labels,
								 const std::unordered_map<std::string, Value> &properties = {});
		QueryBuilder &createEdge(const std::string &variable, const std::string &type,
								 const std::string &sourceVariable, const std::string &targetVariable,
								 const std::unordered_map<std::string, Value> &properties = {});
		QueryBuilder &deleteEntities(const std::vector<std::string> &variables, bool detach = false);
		QueryBuilder &setProperty(const std::string &variable, const std::string &key, const Value &value);
		QueryBuilder &addLabel(const std::string &variable, const std::string &label);
		QueryBuilder &remove(const std::string &variable, const std::string &key, bool isLabel = false);

		QueryBuilder &createIndex(const std::string &label, const std::string &property,
								  const std::string &indexName = "");
		QueryBuilder &dropIndex(const std::string &indexName);
		QueryBuilder &dropIndex(const std::string &label, const std::string &property);
		QueryBuilder &showIndexes();

		QueryBuilder &call(const std::string &procedure, const std::vector<Value> &arguments = {});
		QueryBuilder &returning(const std::vector<std::string> &variables);
		QueryBuilder &skip(int64_t offset);
		QueryBuilder &limit(int64_t maximumRows);
		QueryBuilder &orderBy(const std::vector<SortOrder> &items);
		QueryBuilder &unwind(const std::vector<Value> &values, const std::string &alias);

		[[nodiscard]] PreparedQuery build() const;

	private:
		std::unique_ptr<detail::QueryBuilderImpl> impl_;

		QueryBuilder();
		friend class Database;
		friend class Transaction;
		friend detail::QueryBuilderImpl &detail::requireQueryBuilderImpl(QueryBuilder &builder);
	};

	/**
	 * @brief One property column for columnar bulk inserts.
	 *
	 * All columns passed to a bulk columnar insert must have the same number of
	 * values as the requested row count. This shape avoids building a
	 * vector<unordered_map<...>> at the call site for large imports.
	 */
	struct PropertyColumn {
		std::string name;
		std::vector<Value> values;
	};

	/**
	 * @brief Describes the row and field that invalidated a columnar bulk write.
	 *
	 * Both the C++ API and Driver ABI validate a complete batch before it can
	 * publish graph mutations. Callers can use this type to report a stable,
	 * structured diagnostic instead of parsing an implementation error string.
	 */
	class BulkInputError final : public std::invalid_argument {
	public:
		BulkInputError(size_t rowIndex, std::string fieldPath, std::string message) :
				std::invalid_argument(std::move(message)), rowIndex_(rowIndex), fieldPath_(std::move(fieldPath)) {}

		[[nodiscard]] size_t rowIndex() const noexcept { return rowIndex_; }
		[[nodiscard]] const std::string &fieldPath() const noexcept { return fieldPath_; }

	private:
		size_t rowIndex_;
		std::string fieldPath_;
	};

	/**
	 * @class Result
	 * @brief Represents the output of a query execution.
	 */
	class Result {
	public:
		Result();

		/**
		 * @brief Destructor.
		 * MUST be defined in the .cpp file where ResultImpl is fully defined.
		 */
		~Result();

		// Move-only semantics to transfer ownership of the internal result
		Result(Result &&) noexcept;
		Result &operator=(Result &&) noexcept;

		[[nodiscard]] bool hasNext() const;
		void next() const;
		[[nodiscard]] Value get(const std::string &key) const;

		// --- Data Access (By Index) - used by bindings and Driver ABI ---
		/**
		 * @brief Get value by column index (0-based).
		 * @throws std::out_of_range if index is invalid.
		 */
		[[nodiscard]] Value get(int index) const;

		// --- Metadata - used by bindings and Driver ABI ---
		[[nodiscard]] int getColumnCount() const;
		[[nodiscard]] std::string getColumnName(int index) const;

		[[nodiscard]] double getDuration() const;

		[[nodiscard]] bool isSuccess() const;
		[[nodiscard]] std::string getError() const;

	private:
		std::unique_ptr<ResultImpl> impl_;
		friend class Database; // Allow Database to create Results
		friend class Transaction; // Allow Transaction to create Results
		friend Value detail::getTypedResultValue(const Result &result, int index);
	};

	/**
	 * @class Transaction
	 * @brief Provides ACID transaction semantics.
	 * Auto-rolls back if destroyed without commit.
	 */
	class Transaction {
	public:
		~Transaction();

		Transaction(Transaction &&) noexcept;
		Transaction &operator=(Transaction &&) noexcept;
		Transaction(const Transaction &) = delete;
		Transaction &operator=(const Transaction &) = delete;

		Result execute(const std::string &cypher) const;
		Result execute(const std::string &cypher, const std::unordered_map<std::string, Value> &params) const;
		[[nodiscard]] QueryBuilder query() const;
		[[nodiscard]] Result execute(const PreparedQuery &query) const;

		// Transaction-bound direct writes keep their provisional IDs and
		// read-your-writes visibility inside this transaction only.
		[[nodiscard]] int64_t createNode(const std::string &label,
										 const std::unordered_map<std::string, Value> &props = {}) const;
		[[nodiscard]] int64_t createNode(const std::vector<std::string> &labels,
										 const std::unordered_map<std::string, Value> &props = {}) const;
		[[nodiscard]] std::vector<int64_t>
		createNodes(const std::string &label, const std::vector<std::unordered_map<std::string, Value>> &propsList) const;
		[[nodiscard]] std::vector<int64_t>
		createNodesColumnar(const std::string &label, size_t count, const std::vector<PropertyColumn> &columns) const;
		[[nodiscard]] int64_t createEdge(int64_t sourceId, int64_t targetId, const std::string &edgeType,
										 const std::unordered_map<std::string, Value> &props = {}) const;
		[[nodiscard]] std::vector<int64_t>
		createEdges(const std::string &edgeType,
						const std::vector<std::tuple<int64_t, int64_t, std::unordered_map<std::string, Value>>> &edgesList) const;
		[[nodiscard]] std::vector<int64_t>
		createEdgesColumnar(const std::string &edgeType, const std::vector<int64_t> &sourceIds,
								const std::vector<int64_t> &targetIds,
								const std::vector<PropertyColumn> &columns = {}) const;
		[[nodiscard]] std::vector<Node> getShortestPath(int64_t startId, int64_t endId, int maxDepth = 15) const;
		void bfs(int64_t startNodeId, const std::function<bool(const Node &)> &visitor) const;
		void commit();
		void rollback();
		[[nodiscard]] bool isActive() const;
		[[nodiscard]] bool isReadOnly() const;

	private:
		std::unique_ptr<TransactionImpl> impl_;
		friend class Database;
		friend class DatabaseImpl;
		friend class detail::DatabaseBulkInternal;
		Transaction(); // Only Database can create
	};

	/**
	 * @class Database
	 * @brief The main entry point for the database API.
	 */
	class Database {
	public:
		explicit Database(const std::string &path);

		/**
		 * @brief Destructor.
		 * MUST be defined in the .cpp file where DatabaseImpl is fully defined.
		 * Otherwise, std::unique_ptr cannot delete the incomplete type.
		 */
		~Database();

		// Disable copying
		Database(const Database &) = delete;
		Database &operator=(const Database &) = delete;

		void open() const;
		[[nodiscard]] bool openIfExists() const;
		void close() const;
		void save() const; // Flushes data to disk

		// Transaction support
		[[nodiscard]] Transaction beginTransaction();
		// For large direct API imports. Commit atomically publishes one durable
		// native single-file generation through the A/B Superblock pair.
		[[nodiscard]] Transaction beginBulkTransaction();
		[[nodiscard]] Transaction beginReadOnlyTransaction();
		[[nodiscard]] bool hasActiveTransaction() const;

		// Resize the thread pool at runtime (0=auto, 1=single-thread, >1=N threads)
		void setThreadPoolSize(size_t poolSize) const;


		// Execute raw Cypher query (auto-commit: wraps in implicit transaction)
		[[nodiscard]] Result execute(const std::string &cypher) const;

		// Execute parameterized Cypher query
		[[nodiscard]] Result execute(const std::string &cypher,
									 const std::unordered_map<std::string, Value> &params) const;

		/** @brief Starts a typed, storage-neutral native query. */
		[[nodiscard]] QueryBuilder query() const;
		/** @brief Executes a typed query through the native logical runtime. */
		[[nodiscard]] Result execute(const PreparedQuery &query) const;

		// High-performance direct insert APIs

		/**
		 * @brief Create a node with a single label and return its internal ID.
		 * @return The internal ID (int64_t) of the created node.
		 */
		[[nodiscard]] int64_t createNode(const std::string &label,
										 const std::unordered_map<std::string, Value> &props = {}) const;

		/**
		 * @brief Create a node with multiple labels and return its internal ID.
		 * @return The internal ID (int64_t) of the created node.
		 */
		[[nodiscard]] int64_t createNode(const std::vector<std::string> &labels,
										 const std::unordered_map<std::string, Value> &props = {}) const;

		/**
		 * @brief Create multiple nodes in one batch.
		 * @return Vector of internal IDs for the created nodes.
		 */
		[[nodiscard]] std::vector<int64_t>
		createNodes(const std::string &label,
					const std::vector<std::unordered_map<std::string, Value>> &propsList) const;

		/**
		 * @brief Create multiple nodes from property columns.
		 * @return Vector of internal IDs for the created nodes.
		 */
		[[nodiscard]] std::vector<int64_t>
		createNodesColumnar(const std::string &label,
							size_t count,
							const std::vector<PropertyColumn> &columns) const;

		/**
		 * @brief Create an edge between two known internal IDs and return the edge ID.
		 * @throws std::runtime_error If sourceId or targetId does not exist.
		 * @return The internal ID (int64_t) of the created edge.
		 */
		[[nodiscard]] int64_t createEdge(int64_t sourceId, int64_t targetId, const std::string &edgeType,
										 const std::unordered_map<std::string, Value> &props = {}) const;

		/**
		 * @brief Create multiple edges in one batch.
		 * @return Vector of internal IDs for the created edges.
		 */
		[[nodiscard]] std::vector<int64_t>
		createEdges(const std::string &edgeType,
					const std::vector<std::tuple<int64_t, int64_t, std::unordered_map<std::string, Value>>> &edgesList)
				const;

		/**
		 * @brief Create multiple edges from source/target ID columns and property columns.
		 * @return Vector of internal IDs for the created edges.
		 */
		[[nodiscard]] std::vector<int64_t>
		createEdgesColumnar(const std::string &edgeType,
							const std::vector<int64_t> &sourceIds,
							const std::vector<int64_t> &targetIds,
							const std::vector<PropertyColumn> &columns = {}) const;

		/**
		 * @brief Create multiple node property indexes with one coordinated build.
		 * @return true when every requested index was created.
		 */
		[[nodiscard]] bool createNodePropertyIndexes(const std::string &label,
													 const std::vector<std::string> &properties) const;

		/**
		 * @brief Finds the shortest path between two nodes.
		 * @return Ordered path of nodes.
		 */
		[[nodiscard]] std::vector<Node> getShortestPath(int64_t startId, int64_t endId, int maxDepth = 15) const;

		void bfs(int64_t startNodeId, const std::function<bool(const Node &)> &visitor) const;

	private:
		std::unique_ptr<DatabaseImpl> impl_;
		friend class detail::DatabaseBulkInternal;
	};

} // namespace zyx
