/**
 * @file FileStorageLoad.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/FileStorage.h"
#include <fstream>
#include "graph/utils/ChecksumUtils.h"

using namespace graph::storage;

void FileStorage::load(std::unordered_map<uint64_t, Node> &nodes, std::unordered_map<uint64_t, Edge> &edges) {
	std::ifstream file(dbFilePath, std::ios::binary);
	if (!file)
		throw std::runtime_error("Cannot open database file");

	// Read file header
	FileHeader header;
	file.read(reinterpret_cast<char *>(&header), sizeof(FileHeader));

	// Validate magic number
	if (memcmp(header.magic, "METRIXDB", 8) != 0) {
	 throw std::runtime_error("Invalid file format");
	}

	// Validate header CRC
	uint32_t calculatedCrc = utils::calculateCrc(&header, offsetof(FileHeader, header_crc));
	if (calculatedCrc != header.header_crc) {
	 throw std::runtime_error("Header CRC mismatch, data corruption detected");
	}

	// Load node segments
	uint64_t currentNodeSegmentOffset = header.node_segment_head;
	while (currentNodeSegmentOffset != 0) {
		SegmentHeader segmentHeader;
		file.seekg(static_cast<std::streamoff>(currentNodeSegmentOffset));
		file.read(reinterpret_cast<char *>(&segmentHeader), sizeof(SegmentHeader));

		// Read and validate CRC of segment data
		uint32_t actualCrc = utils::calculateCrc(&segmentHeader, offsetof(SegmentHeader, segment_crc));
		if (actualCrc != segmentHeader.segment_crc) {
			throw std::runtime_error("Data corruption detected in node segment");
		}

		// Read nodes in the segment
		file.seekg(static_cast<std::streamoff>(currentNodeSegmentOffset + sizeof(SegmentHeader)));
		for (uint32_t i = 0; i < segmentHeader.used; ++i) {
			Node node = Node::deserialize(file);
			nodes.emplace(node.getId(), std::move(node));
		}

		currentNodeSegmentOffset = segmentHeader.next_segment_offset;
	}

	// Load edge segments
	uint64_t currentEdgeSegmentOffset = header.edge_segment_head;
	while (currentEdgeSegmentOffset != 0) {
		SegmentHeader segmentHeader;
		file.seekg(static_cast<std::streamoff>(currentEdgeSegmentOffset));
		file.read(reinterpret_cast<char *>(&segmentHeader), sizeof(SegmentHeader));

		// Read and validate CRC of segment data
		uint32_t actualCrc = utils::calculateCrc(&segmentHeader, offsetof(SegmentHeader, segment_crc));
		if (actualCrc != segmentHeader.segment_crc) {
			throw std::runtime_error("Data corruption detected in edge segment");
		}

		// Read edges in the segment
		file.seekg(static_cast<std::streamoff>(currentEdgeSegmentOffset + sizeof(SegmentHeader)));
		for (uint32_t i = 0; i < segmentHeader.used; ++i) {
			Edge edge = Edge::deserialize(file);
			edges.emplace(edge.getId(), std::move(edge));
		}

		currentEdgeSegmentOffset = segmentHeader.next_segment_offset;
	}

	// Update max IDs
	max_node_id = header.max_node_id;
	max_edge_id = header.max_edge_id;
}