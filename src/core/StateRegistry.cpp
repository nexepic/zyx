/**
 * @file StateRegistry.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/18
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/StateRegistry.hpp"
#include "graph/core/StateChainManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include <iostream>

namespace graph {

    // Initialize static members
    std::shared_ptr<storage::DataManager> StateRegistry::dataManager_ = nullptr;
    std::unordered_map<std::string, std::string> StateRegistry::stateCache_;
    std::mutex StateRegistry::mutex_;

    void StateRegistry::initialize(std::shared_ptr<storage::DataManager> dataManager) {
        std::lock_guard<std::mutex> lock(mutex_);
        dataManager_ = std::move(dataManager);
        loadAllStates();
    }

    std::string StateRegistry::getString(const std::string& key, const std::string& defaultValue) {
        std::lock_guard<std::mutex> lock(mutex_);
        return getStringInternal(key, defaultValue);
    }

    int64_t StateRegistry::getInt(const std::string& key, int64_t defaultValue) {
        std::string strValue = getString(key, "");
        if (strValue.empty()) {
            return defaultValue;
        }

        try {
            return std::stoll(strValue);
        } catch ([[maybe_unused]] const std::exception& e) {
            return defaultValue;
        }
    }

    double StateRegistry::getDouble(const std::string& key, double defaultValue) {
        std::string strValue = getString(key, "");
        if (strValue.empty()) {
            return defaultValue;
        }

        try {
            return std::stod(strValue);
        } catch ([[maybe_unused]] const std::exception& e) {
            return defaultValue;
        }
    }

    bool StateRegistry::getBool(const std::string& key, bool defaultValue) {
        std::string strValue = getString(key, "");
        if (strValue.empty()) {
            return defaultValue;
        }

        // Check for common boolean string representations
        if (strValue == "true" || strValue == "1" || strValue == "yes" || strValue == "on") {
            return true;
        } else if (strValue == "false" || strValue == "0" || strValue == "no" || strValue == "off") {
            return false;
        }

        return defaultValue;
    }

    void StateRegistry::setString(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        setStringInternal(key, value);
    }

    void StateRegistry::setInt(const std::string& key, int64_t value) {
        setString(key, std::to_string(value));
    }

    void StateRegistry::setDouble(const std::string& key, double value) {
        setString(key, std::to_string(value));
    }

    void StateRegistry::setBool(const std::string& key, bool value) {
        setString(key, value ? "true" : "false");
    }

    bool StateRegistry::exists(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return stateCache_.contains(key);
    }

    bool StateRegistry::remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = stateCache_.find(key);
        if (it == stateCache_.end()) {
            return false;
        }

        stateCache_.erase(it);

        return true;
    }

    void StateRegistry::loadAllStates() {
        if (!dataManager_) {
            return;
        }

        // Clear current cache
        stateCache_.clear();

        // Get all states from the database
        // This depends on implementing a method in DataManager to retrieve all states
        auto states = dataManager_->getAllStates();

        for (const auto& state : states) {
            if (state.isActive()) {
                stateCache_[state.getKey()] = state.getDataAsString();
            }
        }
    }

    std::string StateRegistry::getStringInternal(const std::string& key, const std::string& defaultValue) {
        auto it = stateCache_.find(key);
        if (it == stateCache_.end()) {
            return defaultValue;
        }
        return it->second;
    }

    void StateRegistry::setStringInternal(const std::string& key, const std::string& value) {
        stateCache_[key] = value;
    }

} // namespace graph