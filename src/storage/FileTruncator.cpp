/**
 * @file FileTruncator.cpp
 * @brief File truncation by removing unused trailing segments
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/FileTruncator.hpp"
#include <algorithm>
#include <filesystem>
#include "graph/storage/SegmentType.hpp"

namespace graph::storage {

	FileTruncator::FileTruncator(std::shared_ptr<StorageIO> io, std::string fileName,
								 std::shared_ptr<SegmentTracker> tracker) :
		io_(std::move(io)), fileName_(std::move(fileName)), tracker_(std::move(tracker)) {}

	FileTruncator::~FileTruncator() = default;

	bool FileTruncator::truncateFile(file_handle_t nativeFd) const {
		auto truncatableSegments = findTruncatableSegments();

		if (truncatableSegments.empty()) {
			return false;
		}

		std::ranges::sort(truncatableSegments);

		uint64_t newFileSize = truncatableSegments.front();

		if (newFileSize < FILE_HEADER_SIZE) {
			newFileSize = FILE_HEADER_SIZE;
		}

		io_->flushStream();

		if (nativeFd != INVALID_FILE_HANDLE) {
			if (portable_ftruncate(nativeFd, newFileSize) != 0) {
				return false;
			}
			auto stream = io_->getStream();
			stream->clear();
			stream->seekg(0, std::ios::beg);
			stream->seekp(0, std::ios::beg);
		} else {
			auto stream = io_->getStream();
			stream->close();
			std::filesystem::resize_file(fileName_, newFileSize);
			stream->open(fileName_, std::ios::in | std::ios::out | std::ios::binary);
		}

		for (uint64_t offset : truncatableSegments) {
			tracker_->removeFromFreeList(offset);
		}

		return true;
	}

	std::vector<uint64_t> FileTruncator::findTruncatableSegments() const {
		std::vector<uint64_t> result;

		auto freeSegments = tracker_->getFreeSegments();
		std::ranges::sort(freeSegments);

		uint64_t highestActiveOffset = 0;
		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			auto segments = tracker_->getSegmentsByType(type);
			for (const auto &header : segments) {
				uint64_t endOffset = header.file_offset + TOTAL_SEGMENT_SIZE;
				highestActiveOffset = std::max(highestActiveOffset, endOffset);
			}
		}

		for (uint64_t offset : freeSegments) {
			if (offset >= highestActiveOffset) {
				result.push_back(offset);
			}
		}

		return result;
	}

} // namespace graph::storage
