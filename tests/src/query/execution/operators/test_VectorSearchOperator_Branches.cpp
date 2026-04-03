/**
 * @file test_VectorSearchOperator_Branches.cpp
 * @brief Targeted branch tests for VectorSearchOperator.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/execution/operators/VectorSearchOperator.hpp"

namespace fs = std::filesystem;

using graph::Database;
using graph::query::execution::operators::VectorSearchOperator;

class VectorSearchOperatorBranchTest : public ::testing::Test {
protected:
	fs::path dbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<graph::storage::DataManager> dm;
	std::shared_ptr<graph::query::indexes::IndexManager> im;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_vector_search_branches_" + boost::uuids::to_string(uuid) + ".db");
		if (fs::exists(dbPath)) {
			fs::remove(dbPath);
		}

		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		if (fs::exists(dbPath)) {
			fs::remove(dbPath);
		}
	}
};

TEST_F(VectorSearchOperatorBranchTest, NextSkipsNodeResolvedAsInvalidFromSearchResults) {
	const std::string indexName = "vec_skip_zero_id";
	ASSERT_TRUE(im->createVectorIndex(indexName, "Person", "embedding", 2, "L2"));

	auto diskann = im->getVectorIndexManager()->getIndex(indexName);
	ASSERT_NE(diskann, nullptr);

	// Inject an ID that does not exist in DataManager. getNode(123) resolves to an
	// inactive/default node (id == 0), exercising the skip branch in operator::next().
	diskann->insert(123, {1.0f, 2.0f});

	VectorSearchOperator op(dm, im, indexName, 5, {1.0f, 2.0f});
	op.open();

	auto firstBatch = op.next();
	ASSERT_TRUE(firstBatch.has_value());
	EXPECT_TRUE(firstBatch->empty());

	EXPECT_FALSE(op.next().has_value());
	op.close();
}
