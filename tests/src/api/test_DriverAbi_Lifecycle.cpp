#include <filesystem>
#include <string>
#include <gtest/gtest.h>
#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;

class DriverAbiLifecycleTest : public ::testing::Test {
protected:
    std::string dbPath;

    void SetUp() override {
        dbPath = (fs::temp_directory_path() / ("zyx_driver_abi_lifecycle_" + std::to_string(std::rand()))).string();
        cleanup();
    }

    void TearDown() override {
        cleanup();
    }

    void cleanup() {
        std::error_code ec;
        fs::remove_all(dbPath, ec);
        fs::remove(dbPath + "-wal", ec);
    }
};

TEST_F(DriverAbiLifecycleTest, VersionIsStableV1) {
    EXPECT_EQ(zyx_driver_abi_version_major(), 1u);
    EXPECT_EQ(zyx_driver_abi_version_minor(), 0u);
    EXPECT_GE(zyx_driver_abi_version_patch(), 0u);
    ASSERT_NE(zyx_driver_runtime_version(), nullptr);
    EXPECT_NE(std::string(zyx_driver_runtime_version()).empty(), true);
}

TEST_F(DriverAbiLifecycleTest, OpenCloseDatabase) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;

    zyx_driver_status_t status = zyx_driver_db_open(dbPath.c_str(), &db, &error);

    EXPECT_EQ(status, ZYX_DRIVER_OK);
    EXPECT_NE(db, nullptr);
    EXPECT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiLifecycleTest, OpenRejectsNullPathWithStructuredError) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;

    zyx_driver_status_t status = zyx_driver_db_open(nullptr, &db, &error);

    EXPECT_EQ(status, ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(db, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("path"), std::string::npos);
    zyx_driver_error_free(error);
}

TEST_F(DriverAbiLifecycleTest, OpenIfExistsReturnsNotFound) {
    zyx_driver_error_t *error = nullptr;
    zyx_driver_db_t *db = nullptr;

    zyx_driver_status_t status = zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error);

    EXPECT_EQ(status, ZYX_DRIVER_NOT_FOUND);
    EXPECT_EQ(db, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_NOT_FOUND);
    zyx_driver_error_free(error);
}
