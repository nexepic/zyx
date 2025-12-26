/**
 * @file metrix.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/16
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

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
        Result(Result&&) noexcept;
        Result& operator=(Result&&) noexcept;

        bool hasNext();
        void next();
        Value get(const std::string& key) const;

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
        explicit Database(const std::string& path);

        /**
         * @brief Destructor.
         * MUST be defined in the .cpp file where DatabaseImpl is fully defined.
         * Otherwise, std::unique_ptr cannot delete the incomplete type.
         */
        ~Database();

        // Disable copying
        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        void open();
        void close();
        void save(); // Flushes data to disk

        // Execute raw Cypher query
        Result execute(const std::string& cypher);

        // High-performance direct insert APIs
        void createNode(const std::string& label, const std::unordered_map<std::string, Value>& props);

    	void createEdge(const std::string& sourceLabel, const std::string& sourceKey, const Value& sourceVal,
						const std::string& targetLabel, const std::string& targetKey, const Value& targetVal,
						const std::string& edgeLabel, const std::unordered_map<std::string, Value>& props);

    	/**
		 * @brief Creates a node and returns its internal ID immediately.
		 *        Essential for tools that need to link nodes later without re-querying.
		 *
		 * @param label The node label (e.g., "BasicBlock", "Instruction").
		 * @param props The properties.
		 * @return The internal ID (int64_t) of the created node.
		 */
    	int64_t createNodeRetId(const std::string& label, const std::unordered_map<std::string, Value>& props = {});

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
    	void createEdgeById(int64_t sourceId, int64_t targetId,
							const std::string& edgeLabel,
							const std::unordered_map<std::string, Value>& props = {});

    	/**
		 * @brief Finds the shortest path between two nodes.
		 * @return Ordered path of nodes.
		 */
    	std::vector<Node> getShortestPath(int64_t startId, int64_t endId, int maxDepth = 15);

    private:
        std::unique_ptr<DatabaseImpl> impl_;
    };

} // namespace metrix