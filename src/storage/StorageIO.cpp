/**
 * @file StorageIO.cpp
 * @brief Unified cross-platform I/O abstraction implementation
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/StorageIO.hpp"

#include <cstring>
#include "graph/storage/PreadHelper.hpp"

namespace graph::storage {

	StorageIO::StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd) :
		stream_(std::move(stream)), writeFd_(writeFd), readFd_(readFd) {}

	void StorageIO::writeAt(uint64_t offset, const void *buf, size_t size) {
		if (!buf || size == 0) return;

		if (writeFd_ != INVALID_FILE_HANDLE) {
			pwrite_ssize_t written = portable_pwrite(writeFd_, buf, size, static_cast<pwrite_off_t>(offset));
			if (written < 0 || static_cast<size_t>(written) != size) {
				throw std::runtime_error("StorageIO::writeAt pwrite failed at offset " + std::to_string(offset));
			}
		} else {
			stream_->seekp(static_cast<std::streamoff>(offset));
			stream_->write(static_cast<const char *>(buf), static_cast<std::streamsize>(size));
			if (!*stream_) {
				throw std::runtime_error("StorageIO::writeAt fstream write failed at offset " + std::to_string(offset));
			}
		}
	}

	size_t StorageIO::readAt(uint64_t offset, void *buf, size_t size) const {
		if (!buf || size == 0) return 0;

		if (readFd_ != INVALID_FILE_HANDLE) {
			ssize_t bytesRead = portable_pread(readFd_, buf, size, static_cast<int64_t>(offset));
			if (bytesRead < 0) {
				throw std::runtime_error("StorageIO::readAt pread failed at offset " + std::to_string(offset));
			}
			return static_cast<size_t>(bytesRead);
		}

		stream_->seekg(static_cast<std::streamoff>(offset));
		stream_->read(static_cast<char *>(buf), static_cast<std::streamsize>(size));
		auto bytesRead = stream_->gcount();
		stream_->clear(stream_->rdstate() & std::ios::badbit); // Clear eof/fail but preserve badbit
		return static_cast<size_t>(bytesRead);
	}

	uint64_t StorageIO::append(const void *buf, size_t size) {
		if (!buf || size == 0) {
			throw std::invalid_argument("StorageIO::append called with null buffer or zero size");
		}

		if (writeFd_ != INVALID_FILE_HANDLE) {
			uint64_t offset = 0;
			if (portable_file_size(writeFd_, &offset) != 0) {
				throw std::runtime_error("StorageIO::append native file-size query failed");
			}
			pwrite_ssize_t written = portable_pwrite(writeFd_, buf, size, static_cast<pwrite_off_t>(offset));
			if (written < 0 || static_cast<size_t>(written) != size) { // ZYX_COV_EXCL_LINE: short native writes are OS-level defensive errors.
				throw std::runtime_error("StorageIO::append native write failed");
			}
			return offset;
		}

		stream_->seekp(0, std::ios::end);
		auto offset = static_cast<uint64_t>(stream_->tellp());
		stream_->write(static_cast<const char *>(buf), static_cast<std::streamsize>(size));
		if (!*stream_) {
			throw std::runtime_error("StorageIO::append fstream write failed");
		}
		return offset;
	}

	uint64_t StorageIO::reserveAppendSpace(size_t size) {
		if (size == 0) {
			throw std::invalid_argument("StorageIO::reserveAppendSpace called with zero size");
		}

		if (writeFd_ != INVALID_FILE_HANDLE) {
			uint64_t offset = 0;
			if (portable_file_size(writeFd_, &offset) != 0) {
				throw std::runtime_error("StorageIO::reserveAppendSpace native file-size query failed");
			}
			const char zero = 0;
			const auto endOffset = static_cast<pwrite_off_t>(offset + size - 1);
			pwrite_ssize_t written = portable_pwrite(writeFd_, &zero, 1, endOffset);
			if (written != 1) { // ZYX_COV_EXCL_LINE: short native file-extension writes are OS-level defensive errors.
				throw std::runtime_error("StorageIO::reserveAppendSpace native file extension failed");
			}
			return offset;
		}

		stream_->clear();
		stream_->seekp(0, std::ios::end);
		auto endPos = stream_->tellp();
		if (endPos < 0) {
			throw std::runtime_error("StorageIO::reserveAppendSpace failed to determine file end");
		}

		auto offset = static_cast<uint64_t>(endPos);
		stream_->seekp(static_cast<std::streamoff>(offset + size - 1));
		const char zero = 0;
		stream_->write(&zero, 1);
		if (!*stream_) { // ZYX_COV_EXCL_LINE: tellp failure is the portable fstream error path covered above.
			throw std::runtime_error("StorageIO::reserveAppendSpace fstream write failed");
		}
		stream_->flush();
		return offset;
	}

	void StorageIO::sync() {
		if (writeFd_ != INVALID_FILE_HANDLE) {
			portable_fsync(writeFd_);
		} else if (stream_) {
			stream_->flush();
		}
	}

	void StorageIO::flushStream() {
		if (stream_) {
			stream_->flush();
		}
	}

} // namespace graph::storage
