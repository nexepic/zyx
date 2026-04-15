/**
 * @file IntegrityChecker.hpp
 * @brief Diagnostic verification logic for segment bitmap consistency and CRC integrity
 *
 * Extracted from FileStorage to separate diagnostic concerns from lifecycle management.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace graph::storage {

	class SegmentTracker;
	class StorageIO;

	class IntegrityChecker {
	public:
		IntegrityChecker(std::shared_ptr<SegmentTracker> tracker,
						 std::shared_ptr<StorageIO> io);

		~IntegrityChecker() = default;

		IntegrityChecker(const IntegrityChecker &) = delete;
		IntegrityChecker &operator=(const IntegrityChecker &) = delete;

		struct SegmentVerifyResult {
			uint64_t offset = 0;
			int64_t startId = 0;
			uint32_t capacity = 0;
			uint32_t dataType = 0;
			bool passed = false;
		};

		struct IntegrityResult {
			bool allPassed = true;
			std::vector<SegmentVerifyResult> segments;
		};

		bool verifyBitmapConsistency(uint64_t segmentOffset) const;
		[[nodiscard]] IntegrityResult verifyIntegrity() const;

	private:
		std::shared_ptr<SegmentTracker> tracker_;
		std::shared_ptr<StorageIO> io_;
	};

} // namespace graph::storage
