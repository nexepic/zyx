/**
 * @file test_RelocatablePublicApiAlgorithms.cpp
 * @brief Public graph-algorithm coverage over the relocatable logical read boundary.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "zyx/zyx.hpp"

using namespace graph;
using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock superblock;
		superblock.database_id = DatabaseId(0x0A0B0C0D0E0F1011ULL);
		superblock.generation = Generation(1U);
		superblock.checkpoint_lsn = LogSequenceNumber(1U);
		return superblock;
	}

	class RelocatablePublicApiAlgorithmsTest : public ::testing::Test {
	protected:
		std::filesystem::path data_path_;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			data_path_ = std::filesystem::temp_directory_path() / ("relocatable_public_algorithms_" + to_string(uuid) + ".dat");
		}

		void TearDown() override {
			std::error_code error;
			std::filesystem::remove(data_path_, error);
		}

		void materializeEmptyRelocatableDatabase() {
			auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary |
																				std::ios::trunc);
			ASSERT_TRUE(data->is_open());
			const file_handle_t dataWrite = portable_open_rw(data_path_.string().c_str());
			const file_handle_t dataRead = portable_open(data_path_.string().c_str(), O_RDONLY);
			ASSERT_NE(dataWrite, INVALID_FILE_HANDLE);
			ASSERT_NE(dataRead, INVALID_FILE_HANDLE);

			auto io = std::make_shared<StorageIO>(data, dataWrite, dataRead);
			auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
			metadata.reset();
			io->sync();
			io.reset();
			static_cast<void>(portable_close_rw(dataWrite));
			static_cast<void>(portable_close(dataRead));
			data->close();
		}
	};

} // namespace

TEST_F(RelocatablePublicApiAlgorithmsTest, ShortestPathAndBfsUseTheBoundedLogicalReadBoundary) {
	materializeEmptyRelocatableDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const int64_t start = database.createNode("AlgorithmNode", {{"name", std::string("start")}});
	const int64_t middle = database.createNode("AlgorithmNode", {{"name", std::string("middle")}});
	const int64_t end = database.createNode("AlgorithmNode", {{"name", std::string("end")}});
	static_cast<void>(database.createEdge(start, middle, "NEXT"));
	static_cast<void>(database.createEdge(middle, end, "NEXT"));

	const std::vector<zyx::Node> path = database.getShortestPath(start, end, 2);
	ASSERT_EQ(path.size(), 3U);
	EXPECT_EQ(path[0].id, start);
	EXPECT_EQ(path[1].id, middle);
	EXPECT_EQ(path[2].id, end);
	EXPECT_EQ(std::get<std::string>(path[0].properties.at("name")), "start");
	EXPECT_EQ(std::get<std::string>(path[2].properties.at("name")), "end");
	EXPECT_TRUE(database.getShortestPath(start, end, 1).empty());
	const std::vector<zyx::Node> selfPath = database.getShortestPath(start, start, 0);
	ASSERT_EQ(selfPath.size(), 1U);
	EXPECT_EQ(selfPath.front().id, start);
	EXPECT_TRUE(database.getShortestPath(999'999, end).empty());

	std::vector<int64_t> visited;
	database.bfs(start, [&visited](const zyx::Node &node) {
		visited.push_back(node.id);
		return true;
	});
	EXPECT_EQ(visited, (std::vector<int64_t>{start, middle, end}));

	std::vector<int64_t> early;
	database.bfs(start, [&early](const zyx::Node &node) {
		early.push_back(node.id);
		return false;
	});
	EXPECT_EQ(early, (std::vector<int64_t>{start}));

	std::vector<int64_t> missing;
	EXPECT_NO_THROW(database.bfs(999'999, [&missing](const zyx::Node &node) {
		missing.push_back(node.id);
		return true;
	}));
	EXPECT_TRUE(missing.empty());
	database.close();
}

TEST_F(RelocatablePublicApiAlgorithmsTest, CreatesPublicNodePropertyIndexBatchesThroughTheLogicalSchemaBoundary) {
	materializeEmptyRelocatableDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	static_cast<void>(database.createNodes(
			"IndexedNode",
			{{{"id", std::string("first")}, {"score", int64_t{7}}},
			 { {"id", std::string("second")}, {"score", int64_t{9}}}}));
	EXPECT_TRUE(database.createNodePropertyIndexes("IndexedNode", {"id", "score"}));

	auto transaction = database.beginTransaction();
	ASSERT_TRUE(transaction.isActive());
	EXPECT_THROW((void)database.createNodePropertyIndexes("IndexedNode", {"another"}), std::runtime_error);
	transaction.rollback();
	database.close();

	RelocatableStorageBackendOptions options;
	options.data_path = data_path_;
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options);
	backend.open();
	const std::vector<LogicalGraphIndexDefinition> definitions = backend.listLogicalIndexes(8U);
	ASSERT_EQ(definitions.size(), 2U);
	std::set<std::string> indexedProperties;
	for (const LogicalGraphIndexDefinition &definition: definitions) {
		EXPECT_EQ(definition.label, "IndexedNode");
		EXPECT_EQ(definition.kind, LogicalGraphIndexKind::LGIK_PROPERTY);
		ASSERT_EQ(definition.property_keys.size(), 1U);
		indexedProperties.insert(definition.property_keys.front());
	}
	EXPECT_EQ(indexedProperties, (std::set<std::string>{"id", "score"}));
	backend.close();
}
