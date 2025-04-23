/**
 * @file FileHeaderManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/FileHeaderManager.h"
#include <cstring>
#include <stdexcept>
#include "graph/utils/ChecksumUtils.h"

namespace graph::storage {

	FileHeaderManager::FileHeaderManager(std::shared_ptr<std::fstream> file, FileHeader& header)
	: file_(std::move(file)), fileHeader_(header) {
		if (!file_ || !file_->good()) {
			throw std::runtime_error("Invalid file handle provided to FileHeaderManager");
		}
	}

	FileHeaderManager::~FileHeaderManager() = default;

	void FileHeaderManager::updateFileHeader() const {
		fileHeader_.max_node_id = maxNodeId;
		fileHeader_.max_edge_id = maxEdgeId;
		fileHeader_.max_blob_id = maxBlobId;
	    fileHeader_.header_crc = utils::calculateCrc(&fileHeader_, offsetof(FileHeader, header_crc));

	    // Write header to the file
	    file_->seekp(0);
	    file_->write(reinterpret_cast<char *>(&fileHeader_), sizeof(FileHeader));
	    file_->flush();
	}

	FileHeader FileHeaderManager::readFileHeader() const {
		FileHeader header;

		// Save current position
		auto currentPos = file_->tellg();

		// Go to start of file
		file_->seekg(0, std::ios::beg);
		file_->read(reinterpret_cast<char *>(&header), sizeof(FileHeader));

		if (!file_->good()) {
			throw std::runtime_error("Failed to read file header");
		}

		// Restore original position
		file_->seekg(currentPos);

		return header;
	}

	// void FileHeaderManager::writeFileHeader(const FileHeader &header) {
	// 	// Save current position
	// 	auto currentPos = file_->tellp();
	//
	// 	// Go to start of file
	// 	file_->seekp(0, std::ios::beg);
	// 	file_->write(reinterpret_cast<const char *>(&header), sizeof(FileHeader));
	//
	// 	if (!file_->good()) {
	// 		throw std::runtime_error("Failed to write file header");
	// 	}
	//
	// 	// Restore original position
	// 	file_->seekp(currentPos);
	// }

	void FileHeaderManager::updateFileHeaderMaxIds(std::shared_ptr<SegmentTracker> tracker) {
		// Scan node segments to find max node ID
		uint64_t nodeSegment = tracker->getChainHead(0);
		while (nodeSegment != 0) {
			SegmentHeader segHeader = tracker->readSegmentHeader(nodeSegment);
			int64_t segmentMaxId = segHeader.start_id + segHeader.used - 1;
			// maxNodeId = std::max(maxNodeId, segmentMaxId);
			maxNodeId = segmentMaxId;
			nodeSegment = segHeader.next_segment_offset;
		}

		// Scan edge segments to find max edge ID
		uint64_t edgeSegment = tracker->getChainHead(1);
		while (edgeSegment != 0) {
			SegmentHeader segHeader = tracker->readSegmentHeader(edgeSegment);
			int64_t segmentMaxId = segHeader.start_id + segHeader.used - 1;
			// maxEdgeId = std::max(maxEdgeId, segmentMaxId);
			maxEdgeId = segmentMaxId;
			edgeSegment = segHeader.next_segment_offset;
		}

		// Scan blob segments to find max blob ID (if you track blob IDs similarly)
		uint64_t blobSegment = tracker->getChainHead(3);
		while (blobSegment != 0) {
			SegmentHeader segHeader = tracker->readSegmentHeader(blobSegment);
			int64_t segmentMaxId = segHeader.start_id + segHeader.used - 1;
			// maxBlobId = std::max(maxBlobId, segmentMaxId);
			maxBlobId = segmentMaxId;
			blobSegment = segHeader.next_segment_offset;
		}
	}

	void FileHeaderManager::initializeFileHeader() {
		fileHeader_.header_crc = utils::calculateCrc(&fileHeader_, offsetof(FileHeader, header_crc));

		file_->write(reinterpret_cast<const char*>(&fileHeader_), sizeof(FileHeader));
		file_->flush();

		file_->clear();
	}

	void FileHeaderManager::validateAndReadHeader() {

		// Validate magic number
		if (memcmp(fileHeader_.magic, FILE_HEADER_MAGIC_STRING, 8) != 0) {
			throw std::runtime_error("Invalid file format");
		}

		// Read file header
		file_->seekg(0);
		file_->read(reinterpret_cast<char *>(&fileHeader_), sizeof(FileHeader));

		// Validate header CRC
		uint32_t calculatedCrc = utils::calculateCrc(&fileHeader_, offsetof(FileHeader, header_crc));
		if (calculatedCrc != fileHeader_.header_crc) {
			throw std::runtime_error("Header CRC mismatch, data corruption detected");
		}
	}

	void FileHeaderManager::extractFileHeaderInfo() {
		maxNodeId = fileHeader_.max_node_id;
		maxEdgeId = fileHeader_.max_edge_id;
		maxBlobId = fileHeader_.max_blob_id;
	}

} // namespace graph::storage
