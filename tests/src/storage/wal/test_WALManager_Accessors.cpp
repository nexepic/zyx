/**
 * @file test_WALManager_Accessors.cpp
 * @brief Accessor-focused tests for WALManager inline methods.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "graph/storage/wal/WALManager.hpp"

namespace fs = std::filesystem;
using namespace graph::storage::wal;

TEST(WALManagerAccessorTest, AutoCheckpointAndWalSizeAccessors) {
	auto uuid = boost::uuids::random_generator()();
	const fs::path dbPath = fs::temp_directory_path() / ("test_wal_accessor_" + boost::uuids::to_string(uuid));
	const fs::path walPath = dbPath.string() + "-wal";

	WALManager mgr;
	mgr.open(dbPath.string());
	ASSERT_TRUE(mgr.isOpen());

	const uint64_t initialSize = mgr.getWALSize();
	EXPECT_GT(initialSize, 0UL);

	mgr.setAutoCheckpointThreshold(static_cast<size_t>(initialSize + 1024));
	EXPECT_EQ(mgr.getAutoCheckpointThreshold(), static_cast<size_t>(initialSize + 1024));
	EXPECT_FALSE(mgr.shouldCheckpoint());

	mgr.setAutoCheckpointThreshold(1);
	EXPECT_EQ(mgr.getAutoCheckpointThreshold(), 1UL);
	EXPECT_TRUE(mgr.shouldCheckpoint());

	mgr.writeBegin(1);
	mgr.writeCommit(1);
	EXPECT_GE(mgr.getWALSize(), initialSize);

	mgr.close();
	if (fs::exists(walPath)) {
		fs::remove(walPath);
	}
}
