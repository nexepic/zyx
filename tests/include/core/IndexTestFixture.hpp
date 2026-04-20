/**
 * @file IndexTestFixture.hpp
 * @author Nexepic
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
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
 **/

#pragma once

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/core/Index.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/utils/Serializer.hpp"

using namespace graph;
using namespace graph::storage;

class IndexTest : public ::testing::Test {
protected:
	// --- Comparators ---
	static bool stringComparator(const PropertyValue &a, const PropertyValue &b) {
		return std::get<std::string>(a.getVariant()) < std::get<std::string>(b.getVariant());
	}

	static bool intComparator(const PropertyValue &a, const PropertyValue &b) {
		return std::get<int64_t>(a.getVariant()) < std::get<int64_t>(b.getVariant());
	}

	// --- Test Suite Setup ---
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath_ = std::filesystem::temp_directory_path() /
					  ("test_db_file_index_" + boost::uuids::to_string(uuid) + ".dat");

		database_ = std::make_unique<Database>(testDbPath_.string());
		database_->open();
		dataManager_ = database_->getStorage()->getDataManager();
	}

	static void TearDownTestSuite() {
		dataManager_.reset();
		if (database_) {
			database_->close();
			database_.reset();
			std::error_code ec;
			if (std::filesystem::exists(testDbPath_)) {
				std::filesystem::remove(testDbPath_, ec);
			}
		}
	}

	// --- Per-Test Setup ---
	void SetUp() override {
		// Create fresh nodes for each test to ensure isolation
		Index newLeaf(0, Index::NodeType::LEAF, 1);
		dataManager_->addIndexEntity(newLeaf);
		leafNode_ = newLeaf;

		Index newInternal(0, Index::NodeType::INTERNAL, 1);
		dataManager_->addIndexEntity(newInternal);
		internalNode_ = newInternal;
	}

	void TearDown() override {
		dataManager_->deleteIndex(leafNode_);
		dataManager_->deleteIndex(internalNode_);
	}

	Index leafNode_;
	Index internalNode_;

	static std::shared_ptr<DataManager> dataManager_;
	static std::unique_ptr<Database> database_;
	static std::filesystem::path testDbPath_;
};

// Initialize static members
inline std::shared_ptr<DataManager> IndexTest::dataManager_ = nullptr;
inline std::unique_ptr<Database> IndexTest::database_ = nullptr;
inline std::filesystem::path IndexTest::testDbPath_;
