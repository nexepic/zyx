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
#include "../core/Edge.h"
#include "../core/Node.h"

namespace graph::storage {

	constexpr uint32_t PAGE_SIZE = 4096;
	constexpr uint32_t SEGMENT_SIZE = PAGE_SIZE * 256; // 1M per segment
	constexpr uint32_t NODES_PER_SEGMENT = SEGMENT_SIZE / sizeof(graph::Node);
	constexpr uint32_t EDGES_PER_SEGMENT = SEGMENT_SIZE / sizeof(graph::Edge);

	constexpr size_t FILE_HEADER_SIZE = 512;

	// File header
	struct FileHeader {
		// Basic information
		char magic[8] = {'M', 'E', 'T', 'R', 'I', 'X', 'D', 'B'};
		uint16_t version = 0x0001; // version
		uint32_t page_size = PAGE_SIZE; // Storage block alignment size
		uint64_t node_segment_head = 0; // Offset of the first node segment
		uint64_t edge_segment_head = 0; // Offset of the first edge segment
		uint64_t free_pool_head = 0; // Head of the free pool list

		// Statistics
		uint64_t max_node_id = 0;
		uint64_t max_edge_id = 0;
		uint32_t segment_count = 0; // Total number of segments

		// Checksum
		uint32_t header_crc = 0;

		uint8_t reserved[FILE_HEADER_SIZE -
						 (sizeof(magic) + sizeof(version) + sizeof(page_size) + sizeof(node_segment_head) +
						  sizeof(edge_segment_head) + sizeof(free_pool_head) + sizeof(max_node_id) +
						  sizeof(max_edge_id) + sizeof(segment_count) + sizeof(header_crc))] = {0};
	};

	// Segment metadata (32 bytes at the beginning of each segment)
	struct SegmentHeader {
		uint64_t next_segment_offset = 0; // File offset of the next segment
		uint8_t data_type = 0; // Data type 0: Node 1: Edge
		uint32_t capacity = 0; // Total capacity of the segment (in items)
		uint32_t used = 0; // Number of used items
		uint64_t start_id = 0; // Starting ID
		uint32_t segment_crc = 0; // CRC checksum of the segment data
		uint8_t reserved[7] = {0}; // Alignment padding
	};

	class FileStorage {
	public:
		explicit FileStorage(std::string path);

		void save();

		template<typename T>
		void saveData(std::fstream &file, std::unordered_map<uint64_t, T> &data, uint64_t &segmentHead,
					  uint32_t maxSegmentSize);

		template<typename T>
		void writeSegmentData(std::fstream &file, uint64_t segmentOffset, const std::vector<T> &data,
							  uint32_t usedItems);

		void load(std::unordered_map<uint64_t, Node> &nodes, std::unordered_map<uint64_t, Edge> &edges);

		[[nodiscard]] const std::unordered_map<uint64_t, Node> &getNodes() const;
		[[nodiscard]] const std::unordered_map<uint64_t, Edge> &getEdges() const;

		void addNode(const Node &node);
		void addEdge(const Edge &edge);

		Node &createNode(const std::string &label);
		Edge &createEdge(const Node &from, const Node &to, const std::string &label);

		[[nodiscard]] uint64_t getLastId() const;

		static void beginWrite();
		void commitWrite();
		static void rollbackWrite();

	private:
		std::string dbFilePath;
		std::unordered_map<uint64_t, Node> nodes;
		std::unordered_map<uint64_t, Edge> edges;
		uint64_t max_node_id = 0;
		uint64_t max_edge_id = 0;

		FileHeader currentHeader;

		// Segment management
		std::vector<uint64_t> nodeSegments; // List of node segment offsets
		std::vector<uint64_t> edgeSegments; // List of edge segment offsets

		// Helper methods
		[[nodiscard]] uint64_t findNodePosition(uint64_t id) const;
		[[nodiscard]] uint64_t findEdgePosition(uint64_t id) const;

	protected:
		uint64_t allocateSegment(std::fstream &file, uint8_t type, uint32_t capacity) const;
	};

	class FileStorageTest : public FileStorage {
	public:
	    using FileStorage::FileStorage;
	    using FileStorage::allocateSegment;
    };

} // namespace graph::storage