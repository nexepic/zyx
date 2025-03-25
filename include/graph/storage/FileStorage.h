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
#include "graph/core/Edge.h"
#include "graph/core/Node.h"
#include "DataManager.h"
#include "StorageHeaders.h"

namespace graph::storage {

	class FileStorage {
	public:
		explicit FileStorage(std::string path, size_t cacheSize = 10000);
		~FileStorage();

		void open();
		void close();
		void save();
		void flush();

		template<typename T>
		void saveData(std::fstream &file, std::unordered_map<uint64_t, T> &data, uint64_t &segmentHead,
					  uint32_t maxSegmentSize);

		template<typename T>
		void writeSegmentData(std::fstream &file, uint64_t segmentOffset, const std::vector<T> &data,
							  uint32_t usedItems);

		// Node operations
		uint64_t insertNode(const Node& node);
		[[nodiscard]] uint64_t getNextNodeId() const { return max_node_id + 1; }

		// Edge operations
		uint64_t insertEdge(const Edge& edge);
		[[nodiscard]] uint64_t getNextEdgeId() const { return max_edge_id + 1; }

		[[nodiscard]] uint64_t getLastId() const;
		[[nodiscard]] uint64_t getNodeCount() const;
		[[nodiscard]] uint64_t getEdgeCount() const;

		static void beginWrite();
		void commitWrite();
		static void rollbackWrite();

		// Cache management
		void clearCache();
		void resizeCache(size_t nodeSize, size_t edgeSize);

		// Get a single node by ID (uses cache)
		Node getNode(uint64_t id);

		// Get a single edge by ID (uses cache)
		Edge getEdge(uint64_t id);

		// Batch operations
		std::vector<Node> getNodes(const std::vector<uint64_t>& ids);
		std::vector<Edge> getEdges(const std::vector<uint64_t>& ids);

		std::unordered_map<uint64_t, Node> getAllNodes();
		std::unordered_map<uint64_t, Edge> getAllEdges();

		// Query-specific loading methods
		std::vector<Node> getNodesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);
		std::vector<Edge> getEdgesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);
		std::vector<Edge> findEdgesByNode(uint64_t nodeId, const std::string& direction = "both");
		std::vector<uint64_t> findConnectedNodeIds(uint64_t nodeId, const std::string& direction = "both");

	private:
		std::string dbFilePath;
		std::unordered_map<uint64_t, Node> nodes;
		std::unordered_map<uint64_t, Edge> edges;
		uint64_t max_node_id = 0;
		uint64_t max_edge_id = 0;

		FileHeader currentHeader;

		bool isFileOpen = false;

		// Segment management
		std::vector<uint64_t> nodeSegments; // List of node segment offsets
		std::vector<uint64_t> edgeSegments; // List of edge segment offsets

		std::unique_ptr<DataManager> dataManager;

	protected:
		uint64_t allocateSegment(std::fstream &file, uint8_t type, uint32_t capacity) const;
		void initializeDataManager();
	};

	class FileStorageTest : public FileStorage {
	public:
	    using FileStorage::FileStorage;
	    using FileStorage::allocateSegment;
    };

} // namespace graph::storage