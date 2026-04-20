/**
 * @file IDAllocatorTestFixture.hpp
 * @author Nexepic
 * @date 2025/12/2
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
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/IDAllocator.hpp"

class IDAllocatorTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate unique temporary file path to avoid conflicts between parallel tests
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_id_alloc_" + to_string(uuid) + ".dat");

		// Initialize Database and components
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);
		edgeAllocator = fileStorage->getIDAllocator(graph::EntityType::Edge);
	}

	void TearDown() override {
		nodeAllocator.reset();
		edgeAllocator.reset();
		dataManager.reset();
		fileStorage.reset();
		if (database) {
			database->close();
			database.reset();
		}
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	[[nodiscard]] graph::Node insertNode(const std::string &label) const {
		const int64_t id = nodeAllocator->allocate();

		const int64_t labelId = dataManager->getOrCreateTokenId(label);

		graph::Node node(id, labelId);
		dataManager->addNode(node);
		return node;
	}

	[[nodiscard]] graph::Edge insertEdge(int64_t startId, int64_t endId, const std::string &label) const {
		const int64_t id = edgeAllocator->allocate();
		const int64_t labelId = dataManager->getOrCreateTokenId(label);

		graph::Edge edge(id, startId, endId, labelId);
		dataManager->addEdge(edge);
		return edge;
	}

	void deleteNode(int64_t id) const {
		graph::Node node_to_delete(id, 0);
		dataManager->deleteNode(node_to_delete);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::IDAllocator> nodeAllocator;
	std::shared_ptr<graph::storage::IDAllocator> edgeAllocator;
};
