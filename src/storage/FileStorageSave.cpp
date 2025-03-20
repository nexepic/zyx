/**
 * @file FileStorageSave.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <fstream>
#include <iostream>
#include "graph/storage/FileStorage.h"

#include "graph/utils/ChecksumUtils.h"

using namespace graph::storage;

void FileStorage::save() {
	std::fstream file(dbFilePath, std::ios::binary | std::ios::in | std::ios::out);
	bool isNewFile = false;

	if (!file) {
		// Allow read and write when creating the file (in|out|trunc)
		file.open(dbFilePath, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		if (!file) {
			throw std::runtime_error("Unable to create database file: " + dbFilePath);
		}
		isNewFile = true;
	}

	// If it is a new file, initialize the file header
	if (isNewFile) {
		currentHeader = FileHeader{};
		file.write(reinterpret_cast<const char*>(&currentHeader), sizeof(FileHeader));
		file.flush();
		// Ensure the file pointer is correct after writing
		file.seekg(0);  // Reset read pointer
		file.seekp(0);  // Reset write pointer
	} else {
		file.seekg(0);
		file.read(reinterpret_cast<char*>(&currentHeader), sizeof(FileHeader));
		if (!file) {
			throw std::runtime_error("Unable to read database file header: " + dbFilePath);
		}
	}

	// 1. Save node data
	saveData<Node>(file, nodes, currentHeader.node_segment_head, NODES_PER_SEGMENT);

	// 2. Save edge data
	saveData<Edge>(file, edges, currentHeader.edge_segment_head, EDGES_PER_SEGMENT);

	// Update file header
	currentHeader.header_crc = utils::calculateCrc(&currentHeader, offsetof(FileHeader, header_crc));
	file.seekp(0);
	file.write(reinterpret_cast<char*>(&currentHeader), sizeof(FileHeader));
	file.flush();

	nodes.clear();
	edges.clear();
}

template<typename T>
void FileStorage::saveData(std::fstream &file, std::unordered_map<uint64_t, T> &data,
                          uint64_t &segmentHead, uint32_t itemsPerSegment) {
    if (data.empty()) return;

    // Sort data
    std::vector<T> sortedData;
    sortedData.reserve(data.size());
    for (const auto& [id, item] : data) sortedData.push_back(item);
    std::sort(sortedData.begin(), sortedData.end(),
             [](const T& a, const T& b) { return a.getId() < b.getId(); });

    uint64_t currentSegmentOffset = segmentHead;
    SegmentHeader currentHeader;

    // Locate the last segment
    if (currentSegmentOffset != 0) {
		while (true) {
			file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
            file.read(reinterpret_cast<char*>(&currentHeader), sizeof(SegmentHeader));
            if (currentHeader.next_segment_offset == 0) break;
            currentSegmentOffset = currentHeader.next_segment_offset;
        }
    } else {
    	currentSegmentOffset = allocateSegment(file, T::typeId, itemsPerSegment);
    	segmentHead = currentSegmentOffset;
    	file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
    	file.read(reinterpret_cast<char*>(&currentHeader), sizeof(SegmentHeader));
    }

    auto dataIt = sortedData.begin();
    while (dataIt != sortedData.end()) {
        // Calculate remaining space
        uint32_t remaining = currentHeader.capacity - currentHeader.used;

    	if (remaining == 0) {
    		// Allocate new segment and link
    		uint64_t newOffset = allocateSegment(file, T::typeId, itemsPerSegment);
    		currentHeader.next_segment_offset = newOffset;

    		// Update current segment header
    		file.seekp(static_cast<std::streamoff>(currentSegmentOffset));
    		file.write(reinterpret_cast<char*>(&currentHeader), sizeof(SegmentHeader));

    		// Move to new segment
    		currentSegmentOffset = newOffset;
    		file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
    		file.read(reinterpret_cast<char*>(&currentHeader), sizeof(SegmentHeader));
    		remaining = currentHeader.capacity;
    	}


        // Calculate the number of items to write
        uint32_t writeCount = std::min(remaining, static_cast<uint32_t>(sortedData.end() - dataIt));
        std::vector<T> batch(dataIt, dataIt + writeCount);

        // Write data and update CRC
        writeSegmentData(file, currentSegmentOffset, batch, currentHeader.used);

        // Reload the updated header
        file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
        file.read(reinterpret_cast<char*>(&currentHeader), sizeof(SegmentHeader));
        dataIt += writeCount;
    }
}

uint64_t FileStorage::allocateSegment(std::fstream &file, uint8_t type, uint32_t capacity) const {
	file.seekp(0, std::ios::end);
	if (!file) {
		throw std::runtime_error("Failed to seek to end of file.");
	}

	const uint64_t baseOffset = file.tellp();

	SegmentHeader header;
	header.data_type = type;
	header.capacity = capacity;
	header.used = 0;
	header.next_segment_offset = 0;
	header.start_id = 0;

	// Write segment header
	file.write(reinterpret_cast<const char*>(&header), sizeof(SegmentHeader));
	if (!file) {
		throw std::runtime_error("Failed to write SegmentHeader.");
	}

	// Write empty data area
	size_t itemSize = (type == 0) ? sizeof(Node) : sizeof(Edge);
	size_t dataSize = capacity * itemSize;
	size_t alignedSize = ((dataSize + currentHeader.page_size - 1) / currentHeader.page_size) * currentHeader.page_size;
	std::vector<char> emptyData(alignedSize, 0);
	file.write(emptyData.data(), static_cast<std::streamsize>(alignedSize));
	if (!file) {
		throw std::runtime_error("Failed to write segment data.");
	}

	file.flush();
	return baseOffset;
}

template<typename T>
void FileStorage::writeSegmentData(std::fstream &file, uint64_t segmentOffset,
								  const std::vector<T>& data, uint32_t baseUsed) {
	// Read segment header
	SegmentHeader header;
	file.seekg(static_cast<std::streamoff>(segmentOffset));
	file.read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

	// Calculate item size
	const size_t itemSize = (header.data_type == 0) ? sizeof(Node) : sizeof(Edge);

	// Write data
	uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader) + baseUsed * itemSize;
	file.seekp(static_cast<std::streamoff>(dataOffset));
	for (const auto& item : data) item.serialize(file);

	// Update header
	header.used = baseUsed + data.size();
	if (baseUsed == 0 && !data.empty()) {
		header.start_id = data.front().getId();
	}

	// Calculate and write CRC of segment data
	header.segment_crc = utils::calculateCrc(&header, offsetof(SegmentHeader, segment_crc));

	// Write updated header
	file.seekp(static_cast<std::streamoff>(segmentOffset));
	file.write(reinterpret_cast<const char*>(&header), sizeof(SegmentHeader));
	file.flush();
}