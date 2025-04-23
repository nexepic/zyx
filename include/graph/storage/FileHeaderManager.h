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
#include "StorageHeaders.h"
#include "SegmentTracker.h"

namespace graph::storage {

	class FileHeaderManager {
	public:
		explicit FileHeaderManager(std::shared_ptr<std::fstream> file, FileHeader& header);
		~FileHeaderManager();

		void updateFileHeader() const;

		// Read current file header
		[[nodiscard]] FileHeader readFileHeader() const;

		// Write updated file header
		void writeFileHeader(const FileHeader& header);

		// Update max IDs based on current segment state
		void updateFileHeaderMaxIds(std::shared_ptr<SegmentTracker> tracker);

		void initializeFileHeader();

		void validateAndReadHeader();

		void extractFileHeaderInfo();

		// Getters for max IDs
		[[nodiscard]] int64_t getMaxNodeId() const { return maxNodeId; }
		[[nodiscard]] int64_t getMaxEdgeId() const { return maxEdgeId; }

		int64_t& getMaxNodeIdRef() { return maxNodeId; }
		int64_t& getMaxEdgeIdRef() { return maxEdgeId; }

	private:
		std::shared_ptr<std::fstream> file_;

		FileHeader& fileHeader_;

		int64_t maxNodeId;
		int64_t maxEdgeId;
		int64_t maxPropertyId;
		int64_t maxBlobId;
	};

} // namespace graph::storage