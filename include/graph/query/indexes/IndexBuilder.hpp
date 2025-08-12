/**
 * @file IndexBuilder.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace graph::storage {
    class FileStorage;
    class DataManager;
}

namespace graph::query::indexes {
    class IndexManager;
    class LabelIndex;
    class PropertyIndex;

    /**
     * @class IndexBuilder
     * @brief Responsible for the bulk creation of indexes from existing data.
     *
     * This class iterates through all nodes and edges in the database and populates
     * the specified index structures. It operates in batches to manage memory usage
     * during the build process.
     */
    class IndexBuilder {
    public:
        /**
         * @brief Constructs an IndexBuilder.
         * @param indexManager A shared pointer to the main IndexManager, used to access entity-specific managers.
         * @param storage A shared pointer to the FileStorage for underlying data access.
         */
        IndexBuilder(std::shared_ptr<IndexManager> indexManager,
                       std::shared_ptr<storage::FileStorage> storage);

        /**
         * @brief Destructor.
         */
        ~IndexBuilder();

        // --- Public Build Methods ---

        /**
         * @brief Builds all label and property indexes for all nodes in the database.
         * This will clear any existing node indexes before building.
         * @return true if successful, false otherwise.
         */
        bool buildAllNodeIndexes() const;

        /**
         * @brief Builds all label and property indexes for all edges in the database.
         * This will clear any existing edge indexes before building.
         * @return true if successful, false otherwise.
         */
        bool buildAllEdgeIndexes() const;

        /**
         * @brief Builds an index for a specific property key on all nodes.
         * This will clear any existing index for this specific property on nodes.
         * @param key The property key to index.
         * @return true if successful, false otherwise.
         */
        bool buildNodePropertyIndex(const std::string& key) const;

        /**
         * @brief Builds an index for a specific property key on all edges.
         * This will clear any existing index for this specific property on edges.
         * @param key The property key to index.
         * @return true if successful, false otherwise.
         */
        bool buildEdgePropertyIndex(const std::string& key) const;

    	/**
		 * @brief Retrieves all active ID ranges for nodes from the data manager.
		 * @return A vector of pairs, where each pair is a [start_id, end_id] range.
		 */
    	std::vector<std::pair<int64_t, int64_t>> getNodeIdRanges() const;

    	/**
		 * @brief Retrieves all active ID ranges for edges from the data manager.
		 * @return A vector of pairs, where each pair is a [start_id, end_id] range.
		 */
    	std::vector<std::pair<int64_t, int64_t>> getEdgeIdRanges() const;

    private:
        // --- Private Helper Methods ---

        /**
         * @brief Processes a batch of node IDs, adding them to the provided indexes.
         * @param nodeIds The vector of node IDs to process.
         * @param labelIndex The label index to populate (can be nullptr).
         * @param propertyIndex The property index to populate (can be nullptr).
         * @param propertyKey If not empty, only this property key will be indexed. Otherwise, all properties are indexed.
         */
        void processNodeBatch(const std::vector<int64_t>& nodeIds,
                              const std::shared_ptr<LabelIndex>& labelIndex,
                              const std::shared_ptr<PropertyIndex>& propertyIndex,
                              const std::string& propertyKey = "") const;

        /**
         * @brief Processes a batch of edge IDs, adding them to the provided indexes.
         * @param edgeIds The vector of edge IDs to process.
         * @param labelIndex The label index to populate (can be nullptr).
         * @param propertyIndex The property index to populate (can be nullptr).
         * @param propertyKey If not empty, only this property key will be indexed. Otherwise, all properties are indexed.
         */
        void processEdgeBatch(const std::vector<int64_t>& edgeIds,
                              const std::shared_ptr<LabelIndex>& labelIndex,
                              const std::shared_ptr<PropertyIndex>& propertyIndex,
                              const std::string& propertyKey = "") const;

        // --- Member Variables ---
        std::shared_ptr<IndexManager> indexManager_;
        std::shared_ptr<storage::FileStorage> storage_;
        std::shared_ptr<storage::DataManager> dataManager_;

        // --- Constants ---
        static constexpr size_t BATCH_SIZE = 1000; // Size of batches for processing entities to control memory usage.
    };

} // namespace graph::query::indexes