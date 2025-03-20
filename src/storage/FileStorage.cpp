/**
 * @file FileStorage.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/FileStorage.h"
#include <fstream>
#include <sstream>
#include <zlib.h>
#include "graph/utils/ChecksumUtils.h"
#include "graph/utils/Serializer.h"

using namespace graph::storage;

FileStorage::FileStorage(std::string path) : dbFilePath(std::move(path)) {}

// The method to find the position of a node in the file
uint64_t FileStorage::findNodePosition(uint64_t id) const {
	uint64_t currentSegmentOffset = currentHeader.node_segment_head;
	while (currentSegmentOffset != 0) {
		SegmentHeader header;
		std::ifstream file(dbFilePath, std::ios::binary);
		file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
		file.read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		// Check if the ID is within the range of this segment
		if (id >= header.start_id && id < header.start_id + header.used) {
			const uint64_t base = currentSegmentOffset + sizeof(SegmentHeader);
			return base + (id - header.start_id) * sizeof(Node);
		}

		currentSegmentOffset = header.next_segment_offset;
	}
	throw std::runtime_error("Node ID not found");
}

uint64_t FileStorage::findEdgePosition(uint64_t id) const {
	uint64_t currentSegmentOffset = currentHeader.edge_segment_head;
	while (currentSegmentOffset != 0) {
		SegmentHeader header;
		std::ifstream file(dbFilePath, std::ios::binary);
		file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
		file.read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		// Check if the ID is within the range of this segment
		if (id >= header.start_id && id < header.start_id + header.used) {
			const uint64_t base = currentSegmentOffset + sizeof(SegmentHeader);
			return base + (id - header.start_id) * sizeof(Edge);
		}

		currentSegmentOffset = header.next_segment_offset;
	}
	throw std::runtime_error("Edge ID not found");
}

const std::unordered_map<uint64_t, graph::Node> &FileStorage::getNodes() const { return nodes; }

const std::unordered_map<uint64_t, graph::Edge> &FileStorage::getEdges() const { return edges; }

void FileStorage::addNode(const Node &node) {
	nodes.emplace(node.getId(), node);
	if (node.getId() > max_node_id) {
		max_node_id = node.getId();
	}
}

void FileStorage::addEdge(const Edge &edge) {
	edges.emplace(edge.getId(), edge);
	if (edge.getId() > max_edge_id) {
		max_edge_id = edge.getId();
	}
}

uint64_t FileStorage::getLastId() const { return std::max(max_node_id, max_edge_id); }

graph::Node &FileStorage::createNode(const std::string &label) {
	uint64_t id = max_node_id + 1; // Optimized ID generation
	Node node(id, label);
	addNode(node);
	return nodes[id];
}

graph::Edge &FileStorage::createEdge(const Node& from, const Node& to, const std::string& label) {
	uint64_t id = max_edge_id + 1; // Optimized ID generation
	Edge edge(id, from.getId(), to.getId(), label);
	addEdge(edge);
	return edges[id];
}

void FileStorage::beginWrite() {
	// Implementation for beginning a write transaction
}

void FileStorage::commitWrite() {
    try {
        save();
    } catch (const std::exception& e) {
        // Handle any exceptions that may occur during the save process
        std::cerr << "Error committing write: " << e.what() << std::endl;
        rollbackWrite();
    }
}

void FileStorage::rollbackWrite() {
	// Implementation for rolling back a write transaction
}