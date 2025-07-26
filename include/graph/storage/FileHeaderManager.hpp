/**
 * @file FileHeaderManager.hpp
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
#include "SegmentTracker.hpp"
#include "SegmentType.hpp"
#include "StorageHeaders.hpp"

namespace graph::storage {

	class FileHeaderManager {
	public:
		explicit FileHeaderManager(std::shared_ptr<std::fstream> file, FileHeader &header);
		~FileHeaderManager();

		void flushFileHeader() const;

		// Read current file header
		[[nodiscard]] FileHeader readFileHeader() const;

		// Write updated file header
		void updateFileHeader(const FileHeader &header) const;

		template<typename MaxIdType>
		static void updateMaxIdForSegment(const std::shared_ptr<SegmentTracker> &tracker, SegmentType segmentType,
								   MaxIdType &maxId);

		// Update max IDs based on current segment state
		void updateFileHeaderMaxIds(const std::shared_ptr<SegmentTracker> &tracker);

		void initializeFileHeader() const;

		void validateAndReadHeader() const;

		void extractFileHeaderInfo();

		int64_t &getMaxNodeIdRef() { return maxNodeId; }
		int64_t &getMaxEdgeIdRef() { return maxEdgeId; }
		int64_t &getMaxPropIdRef() { return maxPropId; }
		int64_t &getMaxBlobIdRef() { return maxBlobId; }
		int64_t &getMaxIndexIdRef() { return maxIndexId; }
		int64_t &getMaxStateIdRef() { return maxStateId; }

		[[nodiscard]] FileHeader &getFileHeader() const { return fileHeader_; }

		void updateFileCrc() const;

		[[nodiscard]] bool validateFileCrc() const;

	private:
		std::shared_ptr<std::fstream> file_;

		FileHeader &fileHeader_;

		int64_t maxNodeId = 0;
		int64_t maxEdgeId = 0;
		int64_t maxPropId = 0;
		int64_t maxBlobId = 0;
		int64_t maxIndexId = 0;
		int64_t maxStateId = 0;

		[[nodiscard]] uint32_t calculateFileCrc() const;
	};

} // namespace graph::storage
