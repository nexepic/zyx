/**
 * @file test_SystemConfigManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "graph/core/Database.hpp"
#include "graph/config/SystemConfigManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/log/Log.hpp"

using namespace graph;
using namespace graph::config;
using namespace graph::storage::state;

class SystemConfigManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 1. Generate unique temporary database path
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        testFilePath = std::filesystem::temp_directory_path() / ("test_configMgr_" + to_string(uuid) + ".dat");

        // 2. Initialize Database and infrastructure
        database = std::make_unique<Database>(testFilePath.string());
        database->open();

        // 3. Create the SystemStateManager helper
        stateManager = std::make_shared<SystemStateManager>(
            database->getStorage()->getDataManager()
        );

        // 4. Create the SystemConfigManager instance under test
        configManager = std::make_shared<SystemConfigManager>(stateManager, database->getStorage());

        // 5. Ensure Log is in a known state (INFO/False) before each test
        log::Log::setDebug(false);
    }

    void TearDown() override {
        database->close();
        database.reset();
        if (std::filesystem::exists(testFilePath)) {
            std::filesystem::remove(testFilePath);
        }
        // Reset Log state to avoid side effects on other tests
        log::Log::setDebug(false);
    }

    std::filesystem::path testFilePath;
    std::unique_ptr<Database> database;
    std::shared_ptr<SystemStateManager> stateManager;
    std::shared_ptr<SystemConfigManager> configManager;
};

// Test 1: Verify that default behavior is applied when no config exists.
TEST_F(SystemConfigManagerTest, DefaultBehavior) {
    // Act
    configManager->loadAndApplyAll();

    // Assert: Default log level is INFO (Debug = false)
    EXPECT_FALSE(graph::log::Log::isDebugEnabled())
        << "Log debug should be disabled by default.";
}

// Test 2: Verify enabling Debug mode via configuration.
TEST_F(SystemConfigManagerTest, EnableDebugLevel) {
    // Arrange: Write configuration to persistent state
    stateManager->set<std::string>(
        keys::SYS_CONFIG,
        keys::Config::LOG_LEVEL,
        "DEBUG"
    );

    // Act: Load configuration
    configManager->loadAndApplyAll();

    // Assert
    EXPECT_TRUE(graph::log::Log::isDebugEnabled())
        << "Log debug should be enabled after loading 'DEBUG' config.";
}

// Test 3: Verify Case Insensitivity (e.g., "DeBuG", "debug").
TEST_F(SystemConfigManagerTest, CaseInsensitivity) {
    // Arrange: Mixed case
    stateManager->set<std::string>(
        keys::SYS_CONFIG,
        keys::Config::LOG_LEVEL,
        "DeBuG"
    );

    // Act
    configManager->loadAndApplyAll();

    // Assert
    EXPECT_TRUE(graph::log::Log::isDebugEnabled())
        << "Config loader should handle mixed case 'DeBuG'.";

    // Arrange: Lowercase
    stateManager->set<std::string>(
        keys::SYS_CONFIG,
        keys::Config::LOG_LEVEL,
        "debug"
    );
    configManager->loadAndApplyAll();
    EXPECT_TRUE(graph::log::Log::isDebugEnabled())
        << "Config loader should handle lowercase 'debug'.";
}

// Test 4: Verify switching back to INFO disables debug.
TEST_F(SystemConfigManagerTest, SwitchToInfo) {
    // Arrange: Start with Debug enabled
    log::Log::setDebug(true);

    // Set config to INFO
    stateManager->set<std::string>(
        keys::SYS_CONFIG,
        keys::Config::LOG_LEVEL,
        "INFO"
    );

    // Act
    configManager->loadAndApplyAll();

    // Assert
    EXPECT_FALSE(graph::log::Log::isDebugEnabled())
        << "Log debug should be disabled when config is 'INFO'.";
}

// Test 5: Verify Hot Reload via Observer (onStateUpdated).
// This tests the interaction between DataManager updates and the ConfigManager.
TEST_F(SystemConfigManagerTest, HotReloadViaObserver) {
    // 1. Initial State: Debug OFF
    EXPECT_FALSE(graph::log::Log::isDebugEnabled());

    // 2. Simulate an update to the "sys.config" state entity.
    // We first write the value using StateManager
    stateManager->set<std::string>(
        keys::SYS_CONFIG,
        keys::Config::LOG_LEVEL,
        "DEBUG"
    );

    // 3. Construct dummy State objects to mimic the notification from DataManager.
    // The critical part is that newState.getKey() must match keys::SYS_CONFIG.
    // ID doesn't strictly matter for the logic, but key does.
    State oldState; // Dummy
    State newState(100, keys::SYS_CONFIG, "some_blob_data");

    // Act: Trigger the observer callback manually
    configManager->onStateUpdated(oldState, newState);

    // Assert: The manager should have detected the key match and reloaded configs.
    EXPECT_TRUE(graph::log::Log::isDebugEnabled())
        << "Hot reload failed: onStateUpdated did not trigger config application.";
}

// Test 6: Verify Observer ignores irrelevant state updates.
TEST_F(SystemConfigManagerTest, IgnoreIrrelevantUpdates) {
    // Arrange: Set config to DEBUG in storage, but DON'T apply it yet.
    stateManager->set<std::string>(
        keys::SYS_CONFIG,
        keys::Config::LOG_LEVEL,
        "DEBUG"
    );
    // Ensure runtime is currently False
    log::Log::setDebug(false);

    // Act: Trigger update for a DIFFERENT key (e.g., an index root).
    State oldState;
    State newState(200, "node.index.label", "blob");

    configManager->onStateUpdated(oldState, newState);

    // Assert: Should NOT have reloaded, so Debug should still be FALSE.
    EXPECT_FALSE(graph::log::Log::isDebugEnabled())
        << "Manager reloaded config on unrelated state change.";

    // Sanity check: Trigger with correct key
    State correctState(100, keys::SYS_CONFIG, "blob");
    configManager->onStateUpdated(oldState, correctState);
    EXPECT_TRUE(graph::log::Log::isDebugEnabled());
}

// Test 7: Persistence Check (Write -> Close -> Reopen).
// Ensures that Database::open() correctly utilizes the manager.
TEST_F(SystemConfigManagerTest, PersistenceThroughRestart) {
    // 1. Write Config
    stateManager->set<std::string>(
        keys::SYS_CONFIG,
        keys::Config::LOG_LEVEL,
        "DEBUG"
    );
    // Flush to disk
    database->getStorage()->flush();
    database->close();

    // Reset Log state to prove the new DB instance sets it
    log::Log::setDebug(false);

    // 2. Reopen DB (Simulate Application Restart)
    auto newDatabase = std::make_unique<Database>(testFilePath.string());

    // Act: Database::open() should internally create SystemConfigManager and load configs.
    newDatabase->open();

    // Assert
    EXPECT_TRUE(graph::log::Log::isDebugEnabled())
        << "Configuration was not automatically applied on database startup.";
}