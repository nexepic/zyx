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

		template<typename T>
		void saveData(std::fstream &file, std::unordered_map<uint64_t, T> &data, uint64_t &segmentHead,
					  uint32_t maxSegmentSize);

		template<typename T>
		void writeSegmentData(std::fstream &file, uint64_t segmentOffset, const std::vector<T> &data,
							  uint32_t usedItems);

		uint64_t allocateSegment(std::fstream &file, uint8_t type, uint32_t capacity) const;

		// Node operations
		uint64_t insertNode(const Node &node);
		[[nodiscard]] uint64_t getNextNodeId() const { return max_node_id + 1; }

		// Edge operations
		uint64_t insertEdge(const Edge &edge);
		[[nodiscard]] uint64_t getNextEdgeId() const { return max_edge_id + 1; }

		[[nodiscard]] uint64_t getLastId() const;
		[[nodiscard]] uint64_t getNodeCount() const;
		[[nodiscard]] uint64_t getEdgeCount() const;

		static void beginWrite();
		void commitWrite();
		static void rollbackWrite();

		// Cache management
		void clearCache() const;

		// Get a single node by ID (uses cache)
		Node getNode(uint64_t id);

		// Get a single edge by ID (uses cache)
		Edge getEdge(uint64_t id);

		// Batch operations
		std::vector<Node> getNodes(const std::vector<uint64_t> &ids);
		std::vector<Edge> getEdges(const std::vector<uint64_t> &ids);

		std::unordered_map<uint64_t, Node> getAllNodes();
		std::unordered_map<uint64_t, Edge> getAllEdges();

		// Query-specific loading methods
		std::vector<Node> getNodesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);
		std::vector<Edge> getEdgesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);
		std::vector<Edge> findEdgesByNode(uint64_t nodeId, const std::string &direction = "both");
		std::vector<uint64_t> findConnectedNodeIds(uint64_t nodeId, const std::string &direction = "both");

		// Node property operations
		void updateNodeProperty(uint64_t nodeId, const std::string &key, const PropertyValue &value);
		PropertyValue getNodeProperty(uint64_t nodeId, const std::string &key);
		void removeNodeProperty(uint64_t nodeId, const std::string &key);

		// Edge property operations
		void updateEdgeProperty(uint64_t edgeId, const std::string &key, const PropertyValue &value);
		PropertyValue getEdgeProperty(uint64_t edgeId, const std::string &key);
		void removeEdgeProperty(uint64_t edgeId, const std::string &key);

		// Get all properties for a node
		std::unordered_map<std::string, PropertyValue> getNodeProperties(uint64_t nodeId);

		// Get all properties for an edge
		std::unordered_map<std::string, PropertyValue> getEdgeProperties(uint64_t edgeId);

		uint64_t storeBlob(const std::string& data, const std::string& contentType = "text");

		BlobStore& getBlobStore();

		// isFileOpen getter
		[[nodiscard]] bool isOpen() const { return isFileOpen; }

		void updateEntityProperties();

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

		// Tracks entities that have had property changes
		std::unordered_set<uint64_t> nodePropsDirty;
		std::unordered_set<uint64_t> edgePropsDirty;

		// Helper methods for updating property references
		void updateNodePropertyReferences(std::fstream& file);
		void updateEdgePropertyReferences(std::fstream& file);
	};
} // namespace graph::storage
