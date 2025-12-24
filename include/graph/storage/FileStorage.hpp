/**
 * @file FileStorage.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <atomic>
#include <string>
#include <unordered_map>
#include "DatabaseInspector.hpp"
#include "DeletionManager.hpp"
#include "FileHeaderManager.hpp"
#include "IDAllocator.hpp"
#include "IStorageEventListener.hpp"
#include "StorageHeaders.hpp"
#include "StorageTypes.hpp"
#include "data/DataManager.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "state/SystemStateManager.hpp"

namespace graph::storage {

	class FileStorage {
	public:
		FileStorage(std::string path, size_t cacheSize, OpenMode mode = OpenMode::CREATE_OR_OPEN);
		~FileStorage();

		void open();
		void save();
		void close();
		void flush();

		void initializeComponents();

		void persistSegmentHeaders() const;

		template<typename T>
		void saveData(std::unordered_map<int64_t, T> &data, uint64_t &segmentHead, uint32_t maxSegmentSize);

		template<typename T>
		void writeSegmentData(uint64_t segmentOffset, const std::vector<T> &data, uint32_t usedItems);

		uint64_t allocateSegment(uint32_t type, uint32_t capacity) const;

		// Node operations
		Node insertNode(const std::string &label) const;

		// Edge operations
		Edge insertEdge(const int64_t &from, const int64_t &to, const std::string &label) const;

		void insertProperties(int64_t entityId, uint32_t entityType,
							  const std::unordered_map<std::string, PropertyValue> &properties) const;

		void updateNode(const Node &node) const;
		void updateEdge(const Edge &edge) const;

		void deleteNode(int64_t nodeId);
		void deleteEdge(int64_t edgeId);

		static void beginWrite();
		void commitWrite();
		static void rollbackWrite();

		// Cache management
		void clearCache() const;

		// Get a single node by ID (uses cache)
		Node getNode(int64_t id);

		// Get a single edge by ID (uses cache)
		Edge getEdge(int64_t id);

		std::unordered_map<int64_t, Node> getAllNodes();
		std::unordered_map<int64_t, Edge> getAllEdges();

		template<typename T>
		void updateEntityInPlace(const T &entity, uint64_t knownSegmentOffset = 0);

		template<typename T>
		void deleteEntityOnDisk(const T &entity);

		// Get all properties for a node
		std::unordered_map<std::string, PropertyValue> getNodeProperties(int64_t nodeId);

		// Get all properties for an edge
		std::unordered_map<std::string, PropertyValue> getEdgeProperties(int64_t edgeId);

		// isFileOpen getter
		[[nodiscard]] bool isOpen() const { return isFileOpen; }

		// Verify bitmap consistency for debugging purposes
		bool verifyBitmapConsistency(uint64_t segmentOffset) const;

		[[nodiscard]] std::shared_ptr<SegmentTracker> getSegmentTracker() const { return segmentTracker; }

		[[nodiscard]] std::shared_ptr<DataManager> getDataManager() const { return dataManager; }

		[[nodiscard]] std::shared_ptr<IDAllocator> getIDAllocator() const { return idAllocator; }

		[[nodiscard]] std::shared_ptr<state::SystemStateManager> getSystemStateManager() const {
			return systemStateManager;
		}

		void registerEventListener(std::weak_ptr<IStorageEventListener> listener);

		std::shared_ptr<DatabaseInspector> getInspector() const {
			return std::make_shared<DatabaseInspector>(fileHeader, fileStream, *dataManager);
		}

	private:
		std::string dbFilePath;
		std::unordered_map<uint64_t, Node> nodes;
		std::unordered_map<uint64_t, Edge> edges;

		FileHeader fileHeader;

		bool isFileOpen = false;

		// Single file stream for all operations
		std::shared_ptr<std::fstream> fileStream;

		// cacheSize
		size_t cacheSize;

		OpenMode openMode_;

		std::shared_ptr<IDAllocator> idAllocator;

		// Segment management
		std::vector<uint64_t> nodeSegments; // List of node segment offsets
		std::vector<uint64_t> edgeSegments; // List of edge segment offsets

		std::shared_ptr<FileHeaderManager> fileHeaderManager;

		std::shared_ptr<DataManager> dataManager;

		std::shared_ptr<SpaceManager> spaceManager;

		std::shared_ptr<DatabaseInspector> databaseInspector;

		std::shared_ptr<SegmentTracker> segmentTracker;

		std::shared_ptr<state::SystemStateManager> systemStateManager;

		std::vector<std::weak_ptr<IStorageEventListener>> eventListeners_;
		std::mutex listenerMutex_;

		// Update bitmap for an entity in the segment header
		template<typename EntityType>
		void updateBitmapForEntity(uint64_t segmentOffset, uint64_t entityId, bool isActive);

		// Update bitmap when writing segment data in batch
		void updateSegmentBitmap(uint64_t segmentOffset, uint64_t startId, uint32_t count, bool isActive = true) const;

		// Read the current segment header from disk
		SegmentHeader readSegmentHeader(uint64_t segmentOffset) const;

		// Write updated segment header to disk
		void writeSegmentHeader(uint64_t segmentOffset, const SegmentHeader &header) const;

		std::mutex flushMutex;
		std::atomic<bool> flushInProgress{false};
		std::atomic<bool> deleteOperationPerformed{false};
	};
} // namespace graph::storage
