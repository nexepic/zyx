/**
 * @file LegacyQueryTestFixture.hpp
 * @brief Shared fixture for testing legacy physical-query behavior in isolation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "graph/query/api/QueryResult.hpp"
#include "storage/LegacyPhysicalStorageTestHarness.hpp"

class LegacyQueryTestFixture : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::storage::testing::LegacyPhysicalStorageTestHarness> db;

	void SetUp() override {
		const boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() /
				   ("legacy_query_test_" + boost::uuids::to_string(uuid) + ".dat");
		std::error_code error;
		std::filesystem::remove_all(testFilePath, error);
		std::filesystem::remove_all(testFilePath.string() + "-wal", error);
		db = std::make_unique<graph::storage::testing::LegacyPhysicalStorageTestHarness>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code error;
		std::filesystem::remove_all(testFilePath, error);
		std::filesystem::remove_all(testFilePath.string() + "-wal", error);
	}

	[[nodiscard]] graph::query::QueryResult execute(const std::string &query) const {
		return db->queryEngine()->execute(query);
	}
};
