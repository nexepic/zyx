/**
 * @file FileHeaderManager.cpp
 * @author Nexepic
 * @date 2025/4/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/storage/FileHeaderManager.hpp"
#include <cstring>
#include <stdexcept>
#include "graph/storage/SegmentType.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	FileHeaderManager::FileHeaderManager(std::shared_ptr<std::fstream> file, FileHeader &header) :
		file_(std::move(file)), fileHeader_(header) {
		if (!file_ || !file_->is_open()) {
			throw std::runtime_error("Invalid or closed file handle provided to FileHeaderManager");
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

		file_->clear();

		file_->seekp(0, std::ios::beg);
		file_->write(reinterpret_cast<const char *>(&fileHeader_), sizeof(FileHeader));

		if (file_->fail()) {
			throw std::runtime_error("Physical I/O error: Failed to write file header to disk.");
		}

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
		file_->clear();
		file_->seekp(0, std::ios::beg);

		fileHeader_.data_crc = 0;
		file_->write(reinterpret_cast<const char *>(&fileHeader_), sizeof(FileHeader));
		file_->flush();
	}

	void FileHeaderManager::validateAndReadHeader() const {
		// 1. Reset the header memory
		std::memset(fileHeader_.magic, 0, 8);

		// 2. Clear stream flags and seek to beginning
		file_->clear();
		file_->seekg(0, std::ios::beg);

		// 3. Read the header
		file_->read(reinterpret_cast<char *>(&fileHeader_), sizeof(FileHeader));

		if (file_->fail() || file_->gcount() < static_cast<std::streamsize>(sizeof(FileHeader))) {
			throw std::runtime_error("Database integrity error: File is empty or truncated.");
		}

		// 4. Validate magic string
		if (memcmp(fileHeader_.magic, FILE_HEADER_MAGIC_STRING, 8) != 0) {
			throw std::runtime_error("Invalid file format: Magic number mismatch.");
		}

		// 5. Validate version (v3: segment-level CRC)
		if (fileHeader_.version != 0x00000003) {
			throw std::runtime_error("Unsupported file format version: " + std::to_string(fileHeader_.version)
				+ " (expected " + std::to_string(0x00000003) + ")");
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

	void FileHeaderManager::updateAggregatedCrc(const std::vector<uint32_t> &segmentCrcs) {
		uint32_t aggCrc = 0;
		if (!segmentCrcs.empty()) {
			aggCrc = utils::calculateCrc(segmentCrcs.data(), segmentCrcs.size() * sizeof(uint32_t));
		}
		fileHeader_.data_crc = aggCrc;
	}

} // namespace graph::storage
