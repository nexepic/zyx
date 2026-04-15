/**
 * @file FileTruncator.hpp
 * @brief File truncation by removing unused trailing segments
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "PwriteHelper.hpp"
#include "SegmentTracker.hpp"
#include "StorageHeaders.hpp"

namespace graph::storage {

	class FileTruncator {
	public:
		FileTruncator(std::shared_ptr<std::fstream> file, std::string fileName,
					  std::shared_ptr<SegmentTracker> tracker);
		~FileTruncator();

		/// Truncates the file by removing unused trailing segments.
		/// When @p nativeFd is a valid handle, uses portable_ftruncate().
		/// Otherwise falls back to close -> resize_file -> reopen.
		bool truncateFile(file_handle_t nativeFd = INVALID_FILE_HANDLE) const;

	private:
		std::vector<uint64_t> findTruncatableSegments() const;

		std::shared_ptr<std::fstream> file_;
		std::string fileName_;
		std::shared_ptr<SegmentTracker> tracker_;
	};

} // namespace graph::storage
