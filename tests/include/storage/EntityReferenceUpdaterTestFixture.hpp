/**
 * @file EntityReferenceUpdaterTestFixture.hpp
 * @author Nexepic
 * @date 2025/12/8
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

#include <algorithm>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

// Include project headers
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

using namespace graph::storage;
using namespace graph;

/**
 * @brief Test Fixture for EntityReferenceUpdater.
 * Sets up the full storage engine stack to simulate real-world entity interactions.
 */
class EntityReferenceUpdaterTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header;

	// Core Storage Components
	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	IDAllocators allocators;
	std::shared_ptr<SegmentAllocator> segmentAllocator;

	// High-level Components
	std::shared_ptr<DataManager> dataManager;
	std::shared_ptr<EntityReferenceUpdater> updater;
	std::shared_ptr<state::SystemStateManager> systemStateManager;

	void SetUp() override {
		// 1. Initialize File
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("ref_update_full_" + to_string(uuid) + ".dat");

		file = std::make_shared<std::fstream>(testFilePath,
											  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());

		// Write initial empty header
		file->write(reinterpret_cast<char *>(&header), sizeof(FileHeader));
		file->flush();

		// 2. Initialize Components
		auto storageIO = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		segmentTracker = std::make_shared<SegmentTracker>(storageIO, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);

		std::pair<EntityType, int64_t *> typeMaxPairs[] = {
				{EntityType::Node, &fileHeaderManager->getMaxNodeIdRef()},
				{EntityType::Edge, &fileHeaderManager->getMaxEdgeIdRef()},
				{EntityType::Property, &fileHeaderManager->getMaxPropIdRef()},
				{EntityType::Blob, &fileHeaderManager->getMaxBlobIdRef()},
				{EntityType::Index, &fileHeaderManager->getMaxIndexIdRef()},
				{EntityType::State, &fileHeaderManager->getMaxStateIdRef()},
		};
		for (auto &[type, maxIdPtr] : typeMaxPairs) {
			allocators[static_cast<size_t>(type)] =
					std::make_shared<IDAllocator>(type, segmentTracker, *maxIdPtr);
		}

		segmentAllocator =
				std::make_shared<SegmentAllocator>(storageIO, segmentTracker, fileHeaderManager);

		dataManager = std::make_shared<DataManager>(file, 100, header, allocators, segmentTracker);
		dataManager->initialize(); // This initializes EntityReferenceUpdater internally

		systemStateManager = std::make_shared<state::SystemStateManager>(dataManager);

		dataManager->setSystemStateManager(systemStateManager);

		// Retrieve the updater from DataManager (created during DataManager::initialize)
		updater = dataManager->getEntityReferenceUpdater();
	}

	void TearDown() override {
		updater.reset();
		dataManager.reset();
		segmentAllocator.reset();
		for (auto &alloc : allocators) alloc.reset();
		fileHeaderManager.reset();
		segmentTracker.reset();
		if (file && file->is_open())
			file->close();
		file.reset();
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	// --- Helpers to create valid on-disk entities and sync Tracker ---
	// These helpers ensure that the SegmentTracker's bitmap matches the DataManager's state,
	// which is crucial for EntityReferenceUpdater to locate entities.

	[[nodiscard]] Node createNode(const std::string &label) const {
		int64_t id = allocators[Node::typeId]->allocate();
		uint64_t offset = segmentTracker->getSegmentOffsetForNodeId(id);
		if (offset == 0)
			offset = segmentAllocator->allocateSegment(Node::typeId, NODES_PER_SEGMENT);

		int64_t labelId = dataManager->getOrCreateTokenId(label);
		Node node(id, labelId);

		dataManager->addNode(node);

		// Manual Tracker Sync
		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		segmentTracker->setEntityActive(offset, id - h.start_id, true);
		if ((id - h.start_id) >= h.used) {
			segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
		}

		return node;
	}

	[[nodiscard]] Edge createEdge(int64_t src, int64_t dst, const std::string &label) const {
		int64_t id = allocators[Edge::typeId]->allocate();
		uint64_t offset = segmentTracker->getSegmentOffsetForEdgeId(id);
		if (offset == 0)
			offset = segmentAllocator->allocateSegment(Edge::typeId, EDGES_PER_SEGMENT);

		int64_t labelId = dataManager->getOrCreateTokenId(label);
		Edge edge(id, src, dst, labelId);

		dataManager->addEdge(edge);

		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		segmentTracker->setEntityActive(offset, id - h.start_id, true);
		if ((id - h.start_id) >= h.used) {
			segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
		}

		return edge;
	}

	[[nodiscard]] Property createProperty(int64_t ownerId, uint32_t ownerType) const {
		int64_t id = allocators[Property::typeId]->allocate();
		uint64_t offset = segmentTracker->getSegmentOffsetForPropId(id);
		if (offset == 0)
			offset = segmentAllocator->allocateSegment(Property::typeId, PROPERTIES_PER_SEGMENT);

		Property prop;
		prop.setId(id);
		prop.setEntityInfo(ownerId, ownerType);
		dataManager->addPropertyEntity(prop);

		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		segmentTracker->setEntityActive(offset, id - h.start_id, true);
		if ((id - h.start_id) >= h.used) {
			segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
		}

		return prop;
	}

	[[nodiscard]] Blob createBlob(int64_t ownerId, uint32_t ownerType) const {
		int64_t id = allocators[Blob::typeId]->allocate();
		uint64_t offset = segmentTracker->getSegmentOffsetForBlobId(id);
		if (offset == 0)
			offset = segmentAllocator->allocateSegment(Blob::typeId, BLOBS_PER_SEGMENT);

		Blob blob;
		blob.setId(id);
		blob.setEntityInfo(ownerId, ownerType);
		dataManager->addBlobEntity(blob);

		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		segmentTracker->setEntityActive(offset, id - h.start_id, true);
		if ((id - h.start_id) >= h.used) {
			segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
		}

		return blob;
	}

	[[nodiscard]] Index createIndex(Index::NodeType nodeType) const {
		int64_t id = allocators[Index::typeId]->allocate();
		uint64_t offset = segmentTracker->getSegmentOffsetForIndexId(id);
		if (offset == 0)
			offset = segmentAllocator->allocateSegment(Index::typeId, INDEXES_PER_SEGMENT);

		Index idx(id, nodeType, 1);
		dataManager->addIndexEntity(idx);

		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		segmentTracker->setEntityActive(offset, id - h.start_id, true);
		if ((id - h.start_id) >= h.used) {
			segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
		}
		return idx;
	}

	[[nodiscard]] State createState(const std::string &key) const {
		int64_t id = allocators[State::typeId]->allocate();
		uint64_t offset = segmentTracker->getSegmentOffsetForStateId(id);
		if (offset == 0)
			offset = segmentAllocator->allocateSegment(State::typeId, STATES_PER_SEGMENT);

		State st(id, key, "data");
		dataManager->addStateEntity(st);

		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		segmentTracker->setEntityActive(offset, id - h.start_id, true);
		if ((id - h.start_id) >= h.used) {
			segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
		}
		return st;
	}
};
