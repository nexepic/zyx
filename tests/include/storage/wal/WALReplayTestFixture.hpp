/**
 * @file WALReplayTestFixture.hpp
 * @date 2026/4/14
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <set>
#include "graph/core/Blob.hpp"
#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/wal/WALManager.hpp"
#include "graph/storage/wal/WALRecord.hpp"
#include "graph/storage/wal/WALRecovery.hpp"
#include "graph/storage/data/EntityChangeType.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage;
using namespace graph::storage::wal;

/**
 * Helper: create a unique temp DB path.
 */
inline fs::path makeTempDbPath() {
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	return fs::temp_directory_path() / ("test_walreplay_" + boost::uuids::to_string(uuid) + ".graph");
}

class WALReplayTest : public ::testing::Test {
protected:
	void SetUp() override {
		dbPath = makeTempDbPath();
		fs::remove_all(dbPath);
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove_all(dbPath, ec);
		std::string walPath = dbPath.string() + "-wal";
		fs::remove(walPath, ec);
	}

	fs::path dbPath;
};
