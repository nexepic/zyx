/**
 * @file ApiTestFixture.hpp
 * @author Nexepic
 * @date 2026/1/5
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

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "zyx/zyx.hpp"
#include "zyx/value.hpp"

namespace fs = std::filesystem;

class CppApiTest : public ::testing::Test {
protected:
	std::string dbPath;
	std::unique_ptr<zyx::Database> db;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		auto now = std::chrono::steady_clock::now().time_since_epoch().count();
		dbPath = (tempDir / ("api_test_" + std::to_string(now) + "_" + std::to_string(std::rand()))).string();
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);

		db = std::make_unique<zyx::Database>(dbPath);
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);
	}
};
