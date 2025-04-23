/**
 * @file FileStorage.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <string>
#include <unordered_map>
#include "DataManager.h"
#include "DatabaseInspector.h"
#include "DeletionManager.h"
#include "FileHeaderManager.h"
#include "IDAllocator.h"
#include "StorageHeaders.h"
#include "graph/core/Edge.h"
#include "graph/core/Node.h"

namespace graph::storage {

	class FileStorage {
	public:
		explicit FileStorage(std::string path, size_t cacheSize = 10000);
		~FileStorage();

		void open();
		void close();
		void save();
		void flush();

		void initializeComponents();

		void persistSegmentHeaders();

		template<typename T>
		void saveData(std::unordered_map<int64_t, T> &data, uint64_t &segmentHead, uint32_t maxSegmentSize);

		template<typename T>
		void writeSegmentData(uint64_t segmentOffset, const std::vector<T> &data, uint32_t usedItems);

		uint64_t allocateSegment(uint8_t type, uint32_t capacity) const;

		// Node operations
		Node insertNode(const std::string &label);

		// Edge operations
		Edge insertEdge(const int64_t &from, const int64_t &to, const std::string &label);

		void updateNode(const Node& node);
		void updateEdge(const Edge& edge);

		bool deleteNode(uint64_t nodeId, bool cascadeEdges = false);
		bool deleteEdge(uint64_t edgeId);

		static void beginWrite();
		void commitWrite();
		static void rollbackWrite();

		// Cache management
		void clearCache() const;

		// Get a single node by ID (uses cache)
		Node getNode(int64_t id);

		// Get a single edge by ID (uses cache)
		Edge getEdge(int64_t id);

		// Batch operations
		std::vector<Node> getNodes(const std::vector<int64_t> &ids);
		std::vector<Edge> getEdges(const std::vector<int64_t> &ids);

		std::unordered_map<int64_t, Node> getAllNodes();
		std::unordered_map<int64_t, Edge> getAllEdges();

		// Query-specific loading methods
		std::vector<Node> getNodesInRange(int64_t startId, int64_t endId, size_t limit = 1000);
		std::vector<Edge> getEdgesInRange(int64_t startId, int64_t endId, size_t limit = 1000);
		std::vector<Edge> findEdgesByNode(int64_t nodeId, const std::string &direction = "both");
		std::vector<int64_t> findConnectedNodeIds(int64_t nodeId, const std::string &direction = "both");

		// Node property operations
		void updateNodeProperty(uint64_t nodeId, const std::string &key, const PropertyValue &value);
		PropertyValue getNodeProperty(uint64_t nodeId, const std::string &key);
		void removeNodeProperty(uint64_t nodeId, const std::string &key);

		// Edge property operations
		void updateEdgeProperty(uint64_t edgeId, const std::string &key, const PropertyValue &value);
		PropertyValue getEdgeProperty(uint64_t edgeId, const std::string &key);
		void removeEdgeProperty(uint64_t edgeId, const std::string &key);

		template<typename T>
		void updateEntityInPlace(const T& entity);

		template<typename T>
		void deleteEntityOnDisk(const T &entity);

		// Get all properties for a node
		std::unordered_map<std::string, PropertyValue> getNodeProperties(uint64_t nodeId);

		// Get all properties for an edge
		std::unordered_map<std::string, PropertyValue> getEdgeProperties(uint64_t edgeId);

		uint64_t storeBlob(const std::string &data, const std::string &contentType = "text");

		BlobStore &getBlobStore();

		// isFileOpen getter
		[[nodiscard]] bool isOpen() const { return isFileOpen; }

		std::unique_ptr<DeletionManager> deletionManager;

		// Verify bitmap consistency for debugging purposes
		bool verifyBitmapConsistency(uint64_t segmentOffset);

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

		std::shared_ptr<IDAllocator> idAllocator;

		// Segment management
		std::vector<uint64_t> nodeSegments; // List of node segment offsets
		std::vector<uint64_t> edgeSegments; // List of edge segment offsets

		std::shared_ptr<FileHeaderManager> fileHeaderManager;

		std::unique_ptr<DataManager> dataManager;

		std::shared_ptr<SpaceManager> spaceManager;

		std::shared_ptr<DatabaseInspector> databaseInspector;

		// Update bitmap for an entity in the segment header
		template<typename EntityType>
		void updateBitmapForEntity(uint64_t segmentOffset, uint64_t entityId, bool isActive);

		// Update bitmap when writing segment data in batch
		void updateSegmentBitmap(uint64_t segmentOffset, uint64_t startId, uint32_t count, bool isActive = true);

		// Read the current segment header from disk
		SegmentHeader readSegmentHeader(uint64_t segmentOffset) const;

		// Write updated segment header to disk
		void writeSegmentHeader(uint64_t segmentOffset, const SegmentHeader& header);

		std::mutex flushMutex;
		std::atomic<bool> flushInProgress{false};
		std::atomic<bool> deleteOperationPerformed{false};
	};
} // namespace graph::storage
