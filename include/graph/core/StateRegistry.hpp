/**
 * @file StateRegistry.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/18
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "State.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <unordered_set>

namespace graph {

    namespace storage {
        class DataManager;
    }

    /**
     * Registry providing global access to State entities.
     * Maintains an in-memory cache of all states for fast access.
     */
    class StateRegistry {
    public:
        /**
         * Initializes the state registry with a data manager.
         *
         * @param dataManager The data manager to use for persistence
         */
        static void initialize(std::shared_ptr<storage::DataManager> dataManager);

        /**
         * Gets a state value by key.
         *
         * @param key The state key
         * @param defaultValue Value to return if key not found
         * @return The state value or default if not found
         */
        static std::string getString(const std::string& key, const std::string& defaultValue = "");

        /**
         * Gets a state value as an integer.
         *
         * @param key The state key
         * @param defaultValue Value to return if key not found
         * @return The state value as int or default if not found
         */
        static int64_t getInt(const std::string& key, int64_t defaultValue = 0);

        /**
         * Gets a state value as a double.
         *
         * @param key The state key
         * @param defaultValue Value to return if key not found
         * @return The state value as double or default if not found
         */
        static double getDouble(const std::string& key, double defaultValue = 0.0);

        /**
         * Gets a state value as a boolean.
         *
         * @param key The state key
         * @param defaultValue Value to return if key not found
         * @return The state value as bool or default if not found
         */
        static bool getBool(const std::string& key, bool defaultValue = false);

        /**
         * Sets a string state value.
         *
         * @param key The state key
         * @param value The value to set
         */
        static void setString(const std::string& key, const std::string& value);

        /**
         * Sets an integer state value.
         *
         * @param key The state key
         * @param value The value to set
         */
        static void setInt(const std::string& key, int64_t value);

        /**
         * Sets a double state value.
         *
         * @param key The state key
         * @param value The value to set
         */
        static void setDouble(const std::string& key, double value);

        /**
         * Sets a boolean state value.
         *
         * @param key The state key
         * @param value The value to set
         */
        static void setBool(const std::string& key, bool value);

        /**
         * Checks if a state with the given key exists.
         *
         * @param key The state key
         * @return True if the key exists, false otherwise
         */
        static bool exists(const std::string& key);

        /**
         * Removes a state.
         *
         * @param key The state key
         * @return True if removed, false if not found
         */
        static bool remove(const std::string& key);

        /**
         * Loads all states into memory.
         */
        static void loadAllStates();

        static std::shared_ptr<storage::DataManager> getDataManager() {
			return dataManager_;
		}

    private:
        static std::shared_ptr<storage::DataManager> dataManager_;
        static std::unordered_map<std::string, std::string> stateCache_;
        static std::mutex mutex_;

        // Private methods to implement thread-safe operations
        static std::string getStringInternal(const std::string& key, const std::string& defaultValue);
        static void setStringInternal(const std::string& key, const std::string& value);
    };

} // namespace graph