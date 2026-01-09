/**
 * @file metrix.hpp
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

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include "value.hpp"

namespace metrix {

	// Forward declarations of implementation classes.
	// These are defined ONLY in the .cpp file to keep this header clean.
	class DatabaseImpl;
	class ResultImpl;

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

		bool hasNext() const;
		void next() const;
		Value get(const std::string &key) const;

		// --- Data Access (By Index) - NEW for C API ---
		/**
		 * @brief Get value by column index (0-based).
		 * @throws std::out_of_range if index is invalid.
		 */
		Value get(int index) const;

		// --- Metadata - NEW for C API ---
		int getColumnCount() const;
		std::string getColumnName(int index) const;

		bool isSuccess() const;
		std::string getError() const;

	private:
		std::unique_ptr<ResultImpl> impl_;
		friend class Database; // Allow Database to create Results
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
		bool openIfExists() const;
		void close() const;
		void save() const; // Flushes data to disk

		// Execute raw Cypher query
		Result execute(const std::string &cypher) const;

		// High-performance direct insert APIs
		void createNode(const std::string &label, const std::unordered_map<std::string, Value> &props) const;

		/**
		 * @brief Create multiple nodes in one batch.
		 *        Essential for high-performance data loading tools.
		 */
		void createNodes(const std::string &label,
						 const std::vector<std::unordered_map<std::string, Value>> &propsList) const;

		void createEdge(const std::string &sourceLabel, const std::string &sourceKey, const Value &sourceVal,
						const std::string &targetLabel, const std::string &targetKey, const Value &targetVal,
						const std::string &edgeLabel, const std::unordered_map<std::string, Value> &props) const;

		/**
		 * @brief Creates a node and returns its internal ID immediately.
		 *        Essential for tools that need to link nodes later without re-querying.
		 *
		 * @param label The node label (e.g., "BasicBlock", "Instruction").
		 * @param props The properties.
		 * @return The internal ID (int64_t) of the created node.
		 */
		int64_t createNodeRetId(const std::string &label,
								const std::unordered_map<std::string, Value> &props = {}) const;

		/**
		 * @brief Creates an edge directly between two known internal IDs.
		 *        This bypasses the query parser and index lookups, offering O(1) performance.
		 *
		 * @param sourceId The internal ID of the source node.
		 * @param targetId The internal ID of the target node.
		 * @param edgeLabel The edge type (e.g., "FLOWS_TO").
		 * @param props Edge properties.
		 * @throws std::runtime_error If sourceId or targetId does not exist.
		 */
		void createEdgeById(int64_t sourceId, int64_t targetId, const std::string &edgeLabel,
							const std::unordered_map<std::string, Value> &props = {}) const;

		/**
		 * @brief Finds the shortest path between two nodes.
		 * @return Ordered path of nodes.
		 */
		std::vector<Node> getShortestPath(int64_t startId, int64_t endId, int maxDepth = 15) const;

		void bfs(int64_t startNodeId, const std::function<bool(const Node &)> &visitor) const;

	private:
		std::unique_ptr<DatabaseImpl> impl_;
	};

} // namespace metrix
