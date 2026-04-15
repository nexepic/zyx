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

		stream_->seekp(0, std::ios::end);
		auto offset = static_cast<uint64_t>(stream_->tellp());
		stream_->write(static_cast<const char *>(buf), static_cast<std::streamsize>(size));
		if (!*stream_) {
			throw std::runtime_error("StorageIO::append fstream write failed");
		}
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
