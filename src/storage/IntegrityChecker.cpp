/**
 * @file IntegrityChecker.cpp
 * @brief Diagnostic verification logic implementation
 *
 * Methods moved from FileStorage.cpp with minimal adjustments.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/IntegrityChecker.hpp"
#include <iostream>
#include <vector>
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	IntegrityChecker::IntegrityChecker(std::shared_ptr<SegmentTracker> tracker,
									   std::shared_ptr<StorageIO> io) :
		tracker_(std::move(tracker)), io_(std::move(io)) {}

	bool IntegrityChecker::verifyBitmapConsistency(uint64_t segmentOffset) const {
		if (segmentOffset == 0) {
			return false;
		}

		SegmentHeader header = tracker_->getSegmentHeader(segmentOffset);

		uint32_t inactiveCount = 0;
		for (uint32_t i = 0; i < header.used; i++) {
			if (!tracker_->getBitmapBit(segmentOffset, i)) {
				inactiveCount++;
			}
		}

		if (inactiveCount != header.inactive_count) {
			std::cerr << "Bitmap inconsistency detected: Counted " << inactiveCount
					  << " inactive items but header reports " << header.inactive_count << std::endl;
			return false;
		}

		return true;
	}

	IntegrityChecker::IntegrityResult IntegrityChecker::verifyIntegrity() const {
		IntegrityResult result;

		auto registry = tracker_->getSegmentTypeRegistry();
		for (const auto &type : registry.getAllTypes()) {
			auto segments = tracker_->getSegmentsByType(static_cast<uint32_t>(type));
			for (const auto &header : segments) {
				SegmentVerifyResult segResult;
				segResult.offset = header.file_offset;
				segResult.startId = header.start_id;
				segResult.capacity = header.capacity;
				segResult.dataType = header.data_type;

				uint64_t dataOffset = header.file_offset + sizeof(SegmentHeader);
				std::vector<char> buffer(SEGMENT_SIZE, 0);

				size_t bytesRead = io_->readAt(dataOffset, buffer.data(), SEGMENT_SIZE);

				if (bytesRead > 0) {
					uint32_t computed = utils::calculateCrc(buffer.data(), SEGMENT_SIZE);
					segResult.passed = (computed == header.segment_crc);
				} else {
					segResult.passed = false;
				}

				if (!segResult.passed) {
					result.allPassed = false;
				}

				result.segments.push_back(segResult);
			}
		}

		return result;
	}

} // namespace graph::storage
