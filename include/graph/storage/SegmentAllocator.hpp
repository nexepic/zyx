/**
 * @file SegmentAllocator.hpp
 * @brief Segment allocation, deallocation, and chain management
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "FileHeaderManager.hpp"
#include "IDAllocator.hpp"
#include "SegmentTracker.hpp"
#include "StorageHeaders.hpp"
#include "StorageIO.hpp"

namespace graph::storage {

	class SegmentAllocator {
	public:
		SegmentAllocator(std::shared_ptr<StorageIO> io, std::shared_ptr<SegmentTracker> tracker,
						 std::shared_ptr<FileHeaderManager> fileHeaderManager, std::shared_ptr<IDAllocator> idAllocator);
		~SegmentAllocator();

		static uint64_t findMaxId(uint32_t type, const std::shared_ptr<SegmentTracker> &tracker);
		uint64_t allocateSegment(uint32_t type, uint32_t capacity) const;
		void deallocateSegment(uint64_t offset) const;

		void updateFileHeaderChainHeads() const;

		std::shared_ptr<SegmentTracker> getTracker() const { return segmentTracker_; }

	private:
		std::shared_ptr<StorageIO> io_;
		std::shared_ptr<SegmentTracker> segmentTracker_;
		std::shared_ptr<FileHeaderManager> fileHeaderManager_;
		std::shared_ptr<IDAllocator> idAllocator_;
	};

} // namespace graph::storage
