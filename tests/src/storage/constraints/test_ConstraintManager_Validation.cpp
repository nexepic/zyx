/**
 * @file test_ConstraintManager_Validation.cpp
 * @brief Focused branch tests for ConstraintManager.
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
#include "graph/storage/constraints/ConstraintManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;
using namespace graph::storage::constraints;

class ConstraintManagerValidationTest : public ::testing::Test {
protected:
	std::unique_ptr<graph::Database> db;
	std::shared_ptr<ConstraintManager> manager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	fs::path dbPath;

	void SetUp() override {
		auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_constraint_mgr_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(dbPath)) fs::remove(dbPath);

		db = std::make_unique<graph::Database>(dbPath.string());
		db->open();
		indexManager = db->getQueryEngine()->getIndexManager();
		manager = std::make_shared<ConstraintManager>(db->getStorage(), indexManager);
		manager->initialize();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(dbPath)) fs::remove(dbPath, ec);
	}
};

TEST_F(ConstraintManagerValidationTest, UniqueConstraintRequiresExactlyOneProperty) {
	EXPECT_THROW(
		manager->createConstraint("uq_invalid", "node", "unique", "Person", {}, ""),
		std::runtime_error);
}

TEST_F(ConstraintManagerValidationTest, NotNullConstraintRequiresExactlyOneProperty) {
	EXPECT_THROW(
		manager->createConstraint("nn_invalid", "node", "not_null", "Person", {"a", "b"}, ""),
		std::runtime_error);
}

TEST_F(ConstraintManagerValidationTest, PropertyTypeConstraintRequiresExactlyOneProperty) {
	EXPECT_THROW(
		manager->createConstraint("type_invalid", "node", "property_type", "Person", {}, "INTEGER"),
		std::runtime_error);
}

TEST_F(ConstraintManagerValidationTest, NodeKeyConstraintRequiresAtLeastOneProperty) {
	EXPECT_THROW(
		manager->createConstraint("nk_invalid", "node", "node_key", "Person", {}, ""),
		std::runtime_error);
}

TEST_F(ConstraintManagerValidationTest, UnknownConstraintTypeThrows) {
	EXPECT_THROW(
		manager->createConstraint("unknown_invalid", "node", "unknown_type", "Person", {"x"}, ""),
		std::runtime_error);
}

