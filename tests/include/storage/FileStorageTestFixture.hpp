/**
 * @file FileStorageTestFixture.hpp
 * @author Nexepic
 * @date 2025/3/19
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
#include <thread>
#include <gtest/gtest.h>
#include <zlib.h>
#include "graph/core/Database.hpp"
#include "graph/storage/SegmentIndexManager.hpp"

class FileStorageTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file name using UUID
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_db_file_" + to_string(uuid) + ".dat");

		// Create and initialize Database instead of FileStorage directly
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		// Get FileStorage from Database
		fileStorage = database->getStorage();
	}

	void TearDown() override {
		fileStorage.reset();
		database->close();
		database.reset();
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
};

// Entity test helper traits for TYPED_TEST parameterization
template<typename T>
struct EntityTestHelper;

template<>
struct EntityTestHelper<graph::Node> {
    static graph::Node create(graph::storage::DataManager& dm) {
        graph::Node n(0, 0);
        dm.addNode(n);
        return n;
    }
    static void update(graph::storage::DataManager& dm, graph::Node& e) {
        e.setLabelId(42);
        dm.updateNode(e);
    }
    static void del(graph::storage::DataManager& dm, graph::Node& e) {
        dm.deleteNode(e);
    }
    static graph::Node get(graph::storage::DataManager& dm, int64_t id) {
        return dm.getNode(id);
    }
    static constexpr uint32_t typeId = graph::Node::typeId;
};

template<>
struct EntityTestHelper<graph::Edge> {
    static graph::Edge create(graph::storage::DataManager& dm) {
        // Edge needs two nodes
        graph::Node n1(0, 0), n2(0, 0);
        dm.addNode(n1);
        dm.addNode(n2);
        graph::Edge e(0, n1.getId(), n2.getId(), 0);
        dm.addEdge(e);
        return e;
    }
    static void update(graph::storage::DataManager& dm, graph::Edge& e) {
        e.setTypeId(42);
        dm.updateEdge(e);
    }
    static void del(graph::storage::DataManager& dm, graph::Edge& e) {
        dm.deleteEdge(e);
    }
    static graph::Edge get(graph::storage::DataManager& dm, int64_t id) {
        return dm.getEdge(id);
    }
    static constexpr uint32_t typeId = graph::Edge::typeId;
};

template<>
struct EntityTestHelper<graph::Property> {
    static graph::Property create(graph::storage::DataManager& dm) {
        graph::Property p;
        p.setId(0);
        p.getMutableMetadata().isActive = true;
        dm.addPropertyEntity(p);
        return p;
    }
    static void update(graph::storage::DataManager& dm, graph::Property& e) {
        e.setProperties({{"key1", graph::PropertyValue(2)}});
        dm.updatePropertyEntity(e);
    }
    static void del(graph::storage::DataManager& dm, graph::Property& e) {
        dm.deleteProperty(e);
    }
    static graph::Property get(graph::storage::DataManager& dm, int64_t id) {
        return dm.getProperty(id);
    }
    static constexpr uint32_t typeId = graph::Property::typeId;
};

template<>
struct EntityTestHelper<graph::Blob> {
    static graph::Blob create(graph::storage::DataManager& dm) {
        graph::Blob b;
        b.setId(0);
        b.setData("initial");
        b.getMutableMetadata().isActive = true;
        dm.addBlobEntity(b);
        return b;
    }
    static void update(graph::storage::DataManager& dm, graph::Blob& e) {
        e.setData("updated");
        dm.updateBlobEntity(e);
    }
    static void del(graph::storage::DataManager& dm, graph::Blob& e) {
        dm.deleteBlob(e);
    }
    static graph::Blob get(graph::storage::DataManager& dm, int64_t id) {
        return dm.getBlob(id);
    }
    static constexpr uint32_t typeId = graph::Blob::typeId;
};

template<>
struct EntityTestHelper<graph::Index> {
    static graph::Index create(graph::storage::DataManager& dm) {
        graph::Index idx;
        idx.setId(0);
        idx.getMutableMetadata().isActive = true;
        dm.addIndexEntity(idx);
        return idx;
    }
    static void update(graph::storage::DataManager& dm, graph::Index& e) {
        e.getMutableMetadata().isActive = true;
        dm.updateIndexEntity(e);
    }
    static void del(graph::storage::DataManager& dm, graph::Index& e) {
        dm.deleteIndex(e);
    }
    static graph::Index get(graph::storage::DataManager& dm, int64_t id) {
        return dm.getIndex(id);
    }
    static constexpr uint32_t typeId = graph::Index::typeId;
};

template<>
struct EntityTestHelper<graph::State> {
    static graph::State create(graph::storage::DataManager& dm) {
        graph::State s;
        s.setId(0);
        s.setKey("testkey");
        dm.addStateEntity(s);
        return s;
    }
    static void update(graph::storage::DataManager& dm, graph::State& e) {
        e.setKey("updated");
        dm.updateStateEntity(e);
    }
    static void del(graph::storage::DataManager& dm, graph::State& e) {
        dm.deleteState(e);
    }
    static graph::State get(graph::storage::DataManager& dm, int64_t id) {
        return dm.getState(id);
    }
    static constexpr uint32_t typeId = graph::State::typeId;
};

template<typename T>
class FileStorageTypedTest : public FileStorageTest {};

using AllEntityTypes = ::testing::Types<graph::Node, graph::Edge, graph::Property, graph::Blob, graph::Index, graph::State>;
