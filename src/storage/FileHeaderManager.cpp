/**
 * @file FileHeaderManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/FileHeaderManager.hpp"
#include <cstring>
#include <stdexcept>
#include "graph/storage/SegmentType.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	FileHeaderManager::FileHeaderManager(std::shared_ptr<std::fstream> file, FileHeader &header) :
		file_(std::move(file)), fileHeader_(header) {
		if (!file_ || !file_->good()) {
			throw std::runtime_error("Invalid file handle provided to FileHeaderManager");
		}
	}

	FileHeaderManager::~FileHeaderManager() = default;

	void FileHeaderManager::flushFileHeader() const {
		fileHeader_.max_node_id = maxNodeId;
		fileHeader_.max_edge_id = maxEdgeId;
		fileHeader_.max_prop_id = maxPropId;
		fileHeader_.max_blob_id = maxBlobId;
		fileHeader_.max_index_id = maxIndexId;
		fileHeader_.max_state_id = maxStateId;

		// First, update the header without CRC
		file_->seekp(0);
		file_->write(reinterpret_cast<char *>(&fileHeader_), sizeof(FileHeader));
		file_->flush();

		// Now calculate and update the file-wide CRC
		updateFileCrc();
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

	void FileHeaderManager::updateFileHeader(const FileHeader &header) const { fileHeader_ = header; }

	template<typename MaxIdType>
	void FileHeaderManager::updateMaxIdForSegment(const std::shared_ptr<SegmentTracker> &tracker,
												  SegmentType segmentType, MaxIdType &maxId) {
		uint64_t segment = tracker->getChainHead(toUnderlying(segmentType));
		while (segment != 0) {
			SegmentHeader segHeader = tracker->getSegmentHeader(segment);
			int64_t segmentMaxId = segHeader.start_id + segHeader.used - 1;
			maxId = segmentMaxId; // Update max ID
			segment = segHeader.next_segment_offset;
		}
	}

	void FileHeaderManager::updateFileHeaderMaxIds(const std::shared_ptr<SegmentTracker> &tracker) {
		updateMaxIdForSegment(tracker, SegmentType::Node, maxNodeId);
		updateMaxIdForSegment(tracker, SegmentType::Edge, maxEdgeId);
		updateMaxIdForSegment(tracker, SegmentType::Property, maxPropId);
		updateMaxIdForSegment(tracker, SegmentType::Blob, maxBlobId);
		updateMaxIdForSegment(tracker, SegmentType::Index, maxIndexId);
		updateMaxIdForSegment(tracker, SegmentType::State, maxStateId);
	}

	void FileHeaderManager::initializeFileHeader() const {
		fileHeader_.data_crc = utils::calculateCrc(&fileHeader_, offsetof(FileHeader, data_crc));

		file_->write(reinterpret_cast<const char *>(&fileHeader_), sizeof(FileHeader));
		file_->flush();

		file_->clear();
	}

	void FileHeaderManager::validateAndReadHeader() const {
		// Validate magic number
		if (memcmp(fileHeader_.magic, FILE_HEADER_MAGIC_STRING, 8) != 0) {
			throw std::runtime_error("Invalid file format");
		}

		// Read file header
		file_->seekg(0);
		file_->read(reinterpret_cast<char *>(&fileHeader_), sizeof(FileHeader));

		// Validate file CRC
		try {
			if (!validateFileCrc()) {
				throw std::runtime_error("File CRC mismatch, data corruption detected");
			}
		} catch (const std::runtime_error &e) {
			std::cerr << e.what() << std::endl;
			std::exit(EXIT_FAILURE); // Exit gracefully
		}
	}

	void FileHeaderManager::extractFileHeaderInfo() {
		maxNodeId = fileHeader_.max_node_id;
		maxEdgeId = fileHeader_.max_edge_id;
		maxPropId = fileHeader_.max_prop_id;
		maxBlobId = fileHeader_.max_blob_id;
		maxIndexId = fileHeader_.max_index_id;
		maxStateId = fileHeader_.max_state_id;
	}

	uint32_t FileHeaderManager::calculateFileCrc() const {
		// Save current position
		auto originalPos = file_->tellg();

		// Create a buffer for reading chunks of the file
		constexpr size_t BUFFER_SIZE = 8192; // 8KB buffer
		std::vector<char> buffer(BUFFER_SIZE);

		// Calculate CRC for header (excluding the CRC field itself)
		file_->seekg(0);
		uint32_t crc = 0;

		// Read header part before CRC field
		size_t crcFieldOffset = offsetof(FileHeader, data_crc);

		file_->read(buffer.data(), static_cast<std::streamsize>(crcFieldOffset));
		size_t bytesRead = file_->gcount();
		crc = utils::updateCrc(crc, buffer.data(), bytesRead);

		// Skip CRC field (4 bytes)
		file_->seekg(static_cast<std::streamoff>(crcFieldOffset + sizeof(uint32_t)));

		// Read header part after CRC field
		size_t headerSize = sizeof(FileHeader);
		if (headerSize > crcFieldOffset + sizeof(uint32_t)) {
			size_t remainingHeaderBytes = headerSize - (crcFieldOffset + sizeof(uint32_t));
			file_->read(buffer.data(), static_cast<std::streamsize>(remainingHeaderBytes));
			size_t bytesReadAfterCRC = file_->gcount();
			crc = utils::updateCrc(crc, buffer.data(), bytesReadAfterCRC);
		}

		// Calculate CRC for the rest of the file
		file_->seekg(static_cast<std::streamsize>(headerSize));
		while (*file_) {
			file_->read(buffer.data(), BUFFER_SIZE);
			size_t bytesReadRest = file_->gcount();
			if (bytesReadRest > 0) {
				crc = utils::updateCrc(crc, buffer.data(), bytesReadRest);
			} else {
				break;
			}
		}

		// Restore original position
		file_->clear(); // Clear EOF flag
		file_->seekg(originalPos);

		return crc;
	}

	void FileHeaderManager::updateFileCrc() const {
		// Calculate CRC for the entire file except the CRC field itself
		uint32_t fileCrc = calculateFileCrc();

		// Update the CRC field
		fileHeader_.data_crc = fileCrc;

		// Write back the updated header
		file_->seekp(0);
		file_->write(reinterpret_cast<char *>(&fileHeader_), sizeof(FileHeader));
		file_->flush();
	}

	bool FileHeaderManager::validateFileCrc() const {
		// Calculate the current file CRC
		uint32_t calculatedCrc = calculateFileCrc();

		// Compare with stored CRC
		return calculatedCrc == fileHeader_.data_crc;
	}

} // namespace graph::storage
