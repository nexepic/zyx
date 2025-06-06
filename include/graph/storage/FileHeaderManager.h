/**
 * @file FileHeaderManager.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <fstream>
#include <memory>
#include "SegmentTracker.h"
#include "StorageHeaders.h"

namespace graph::storage {

	class FileHeaderManager {
	public:
		explicit FileHeaderManager(std::shared_ptr<std::fstream> file, FileHeader &header);
		~FileHeaderManager();

		void flushFileHeader() const;

		// Read current file header
		[[nodiscard]] FileHeader readFileHeader() const;

		// Write updated file header
		void updateFileHeader(const FileHeader &header);

		// Update max IDs based on current segment state
		void updateFileHeaderMaxIds(std::shared_ptr<SegmentTracker> tracker);

		void initializeFileHeader();

		void validateAndReadHeader();

		void extractFileHeaderInfo();

		// Getters for max IDs
		[[nodiscard]] int64_t getMaxNodeId() const { return maxNodeId; }
		[[nodiscard]] int64_t getMaxEdgeId() const { return maxEdgeId; }
		[[nodiscard]] int64_t getMaxPropId() const { return maxPropId; }
		[[nodiscard]] int64_t getMaxBlobId() const { return maxBlobId; }

		int64_t &getMaxNodeIdRef() { return maxNodeId; }
		int64_t &getMaxEdgeIdRef() { return maxEdgeId; }
		int64_t &getMaxPropIdRef() { return maxPropId; }
		int64_t &getMaxBlobIdRef() { return maxBlobId; }

		[[nodiscard]] FileHeader &getFileHeader() const { return fileHeader_; }

		void validateChainHeads(std::shared_ptr<SegmentTracker> tracker);

	private:
		std::shared_ptr<std::fstream> file_;

		FileHeader &fileHeader_;

		int64_t maxNodeId;
		int64_t maxEdgeId;
		int64_t maxPropId;
		int64_t maxBlobId;
	};

} // namespace graph::storage
