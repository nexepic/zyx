/**
 * @file test_RelocatableStorageBackendOnlineIndexBounds.cpp
 * @brief Online-index DDL bounds must distinguish scan work from output entries.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"

using namespace graph::storage;

namespace {

	class RelocatableStorageBackendOnlineIndexBoundsTest : public ::testing::Test {
	protected:
		std::filesystem::path data_path_;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			data_path_ = std::filesystem::temp_directory_path() / ("relocatable_online_index_bounds_" + to_string(uuid));
		}

		void TearDown() override {
			std::error_code error;
			std::filesystem::remove(data_path_, error);
		}

		[[nodiscard]] RelocatableStorageBackendOptions options() const {
			RelocatableStorageBackendOptions result;
			result.data_path = data_path_;
			result.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
			result.maximum_index_definitions = 8U;
			result.maximum_online_index_entries = 1U;
			result.maximum_online_index_scan_nodes = 3U;
			result.maximum_online_index_pages = 16U;
			result.online_index_sort_run_bytes = 4096U;
			result.maximum_online_index_sort_runs = 4U;
			return result;
		}
	};

} // namespace

TEST_F(RelocatableStorageBackendOnlineIndexBoundsTest, BuildsSparseHistoricalScalarAndVectorIndexesWithinSeparateBounds) {
	const auto backend = RelocatableStorageBackend::initializeNew(options());
	backend->open();
	const graph::PropertyValue embedding(
			std::vector<graph::PropertyValue>{graph::PropertyValue(1.0), graph::PropertyValue(-2.0)});
	static_cast<void>(backend->createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}, {"embedding", embedding}}},
			 {{"Person"}, {}}}));

	EXPECT_NO_THROW(static_cast<void>(backend->createLogicalNodeIndex({"person_score", "Person", {"score"}})));
	EXPECT_NO_THROW(static_cast<void>(
			backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"})));
	EXPECT_EQ(backend->listLogicalIndexes(8U).size(), 2U);
	EXPECT_NO_THROW(static_cast<void>(backend->verifyIntegrity()));
	backend->close();
}
