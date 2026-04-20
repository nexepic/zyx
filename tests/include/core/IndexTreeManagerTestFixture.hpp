/**
 * @file IndexTreeManagerTestFixture.hpp
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

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/core/IndexTreeManager.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

using namespace graph;
using namespace graph::storage;
using namespace graph::query::indexes;

class IndexTreeManagerTest : public ::testing::Test {
protected:
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath_ = std::filesystem::temp_directory_path() /
					  ("test_indexTreeManager_" + boost::uuids::to_string(uuid) + ".dat");

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

	void SetUp() override {
		// Create managers for specific key types as needed by tests.
		stringTreeManager_ =
				std::make_shared<IndexTreeManager>(dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::STRING);
		stringRootId_ = stringTreeManager_->initialize();

		intTreeManager_ =
				std::make_shared<IndexTreeManager>(dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::INTEGER);
		intRootId_ = intTreeManager_->initialize();
	}

	void TearDown() override {
		if (stringRootId_ != 0)
			stringTreeManager_->clear(stringRootId_);
		if (intRootId_ != 0)
			intTreeManager_->clear(intRootId_);
	}

	static std::string generatePaddedKey(int i, int width = 5) {
		std::ostringstream ss;
		ss << "key_" << std::setw(width) << std::setfill('0') << i;
		return ss.str();
	}

	static std::unique_ptr<Database> database_;
	static std::shared_ptr<DataManager> dataManager_;
	static std::filesystem::path testDbPath_;

	std::shared_ptr<IndexTreeManager> stringTreeManager_;
	int64_t stringRootId_{};
	std::shared_ptr<IndexTreeManager> intTreeManager_;
	int64_t intRootId_{};
};

inline std::unique_ptr<Database> IndexTreeManagerTest::database_ = nullptr;
inline std::shared_ptr<DataManager> IndexTreeManagerTest::dataManager_ = nullptr;
inline std::filesystem::path IndexTreeManagerTest::testDbPath_;
