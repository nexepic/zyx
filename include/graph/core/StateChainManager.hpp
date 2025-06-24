/**
 * @file StateChainManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "State.hpp"
#include <memory>
#include <string>
#include <vector>

namespace graph {

    namespace storage {
        class DataManager;
    }

    /**
     * Manages chains of State entities for storing large configuration data
     * across multiple State records.
     */
    class StateChainManager {
    public:
        explicit StateChainManager(std::shared_ptr<storage::DataManager> dataManager);

        /**
         * Creates a chain of State entities for storing data that may exceed a single State's capacity.
         *
         * @param key Unique identifier for the state
         * @param data The data to store in the chain
         * @return Vector of State entities forming the chain
         */
        [[nodiscard]] std::vector<State> createStateChain(const std::string& key, const std::string& data) const;

        /**
         * Reads the complete data from a chain of State entities.
         *
         * @param headStateId ID of the first State in the chain
         * @return Complete data from all chained States
         */
        [[nodiscard]] std::string readStateChain(int64_t headStateId) const;

        /**
         * Updates an existing State chain with new data.
         *
         * @param headStateId ID of the first State in the chain
         * @param newData New data to store in the chain
         * @return Vector of State entities forming the updated chain
         */
        [[nodiscard]] std::vector<State> updateStateChain(int64_t headStateId, const std::string& newData) const;

        /**
         * Deletes all States in a chain.
         *
         * @param headStateId ID of the first State in the chain
         */
        void deleteStateChain(int64_t headStateId) const;

        /**
         * Gets the IDs of all States in a chain.
         *
         * @param headStateId ID of the first State in the chain
         * @return Vector of all State IDs in the chain
         */
        [[nodiscard]] std::vector<int64_t> getStateChainIds(int64_t headStateId) const;

        /**
         * Finds a State by its key.
         *
         * @param key The key to search for
         * @return The State if found, empty State otherwise
         */
        [[nodiscard]] State findStateByKey(const std::string& key) const;

    private:
        std::shared_ptr<storage::DataManager> dataManager_;

        /**
		 * Splits data into chunks that fit within State entities.
		 *
		 * @param data Data to split
		 * @return Vector of data chunks
		 */
		static std::vector<std::string> splitData(const std::string& data);
    };

} // namespace graph