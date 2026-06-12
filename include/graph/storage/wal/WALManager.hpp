/**
 * @file WALManager.hpp
 * @date 2026/3/19
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include "WALRecord.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/PreadHelper.hpp"

namespace graph::storage {
	class DataManager;
}

namespace graph::storage::wal {

	struct WALEntityChange {
		uint8_t entityType;
		uint8_t changeType;
		int64_t entityId;
		std::vector<uint8_t> serializedData;
	};

	struct WALEntityChangeView {
		uint8_t entityType;
		uint8_t changeType;
		int64_t entityId;
		const uint8_t *serializedData = nullptr;
		uint32_t serializedSize = 0;
	};

	/**
	 * Write-Ahead Log manager for transaction durability.
	 *
	 * Recovery: WALRecovery reads committed-but-unflushed transactions via
	 * readRecords(), replays entity writes to the DB file via StorageWriter,
	 * then calls checkpoint() to truncate the WAL. See WALRecovery.hpp.
	 */
	class WALManager {
	public:
		enum class CloseMode {
			WCM_KEEP_FILE,
			WCM_REMOVE_FILE,
		};

		enum class CloseSyncMode {
			WSM_SYNC,
			WSM_FLUSH_ONLY,
		};

		WALManager() = default;
		~WALManager();

		void open(const std::string &dbPath);
		void close(CloseMode mode = CloseMode::WCM_KEEP_FILE,
				   CloseSyncMode syncMode = CloseSyncMode::WSM_SYNC);

		void writeBegin(uint64_t txnId);
		void writeEntityChange(uint64_t txnId, uint8_t entityType, uint8_t changeType, int64_t entityId,
							   const std::vector<uint8_t> &serializedData);
		void writeEntityChanges(uint64_t txnId, const std::vector<WALEntityChange> &changes);
		void writeEntityChangeViews(uint64_t txnId, std::span<const WALEntityChangeView> changes);
		void writeCommit(uint64_t txnId);
		void writeRollback(uint64_t txnId);
		void sync();
		void checkpoint();

		[[nodiscard]] bool needsRecovery();
		[[nodiscard]] bool isOpen() const { return isOpen_; }
		[[nodiscard]] WALReadResult readRecords();
		[[nodiscard]] std::string getWALPath() const { return walPath_; }

		void setGroupCommitDelayUs(uint32_t delayUs) { groupCommitDelayUs_ = delayUs; }
		[[nodiscard]] uint32_t getGroupCommitDelayUs() const { return groupCommitDelayUs_; }
		void setBufferSize(size_t size) { walBufferSize_ = size; }
		void setAutoCheckpointThreshold(size_t bytes) { autoCheckpointThreshold_ = bytes; }
		[[nodiscard]] size_t getAutoCheckpointThreshold() const { return autoCheckpointThreshold_; }
		[[nodiscard]] uint64_t getWALSize() const { return currentWriteOffset_; }
		[[nodiscard]] bool shouldCheckpoint() const { return currentWriteOffset_ > autoCheckpointThreshold_; }

	private:
		std::string walPath_;
		bool isOpen_ = false;

		// Native file descriptor for pwrite/pread/fsync
		file_handle_t walFd_ = INVALID_FILE_HANDLE;

		// Write buffer
		std::vector<uint8_t> writeBuffer_;
		size_t walBufferSize_ = 1024 * 1024; // 1 MiB default; keeps bulk WAL appends coalesced.

		// Group commit coordination
		std::mutex commitMutex_;
		std::condition_variable commitCV_;
		uint64_t lastSyncedOffset_ = 0;
		uint64_t currentWriteOffset_ = 0;
		bool commitInProgress_ = false;

		// Group commit timing
		uint32_t groupCommitDelayUs_ = 0; // opt-in: TransactionManager serializes writers by default

		// Auto-checkpoint threshold (default 1MB)
		size_t autoCheckpointThreshold_ = 1048576;

		void writeRecord(WALRecordType type, uint64_t txnId, const uint8_t *data = nullptr, uint32_t dataSize = 0);
		void appendRecordLocked(WALRecordType type, uint64_t txnId, const uint8_t *data, uint32_t dataSize);
		void appendEntityChangeRecordLocked(uint64_t txnId, uint8_t entityType, uint8_t changeType, int64_t entityId,
											const uint8_t *data, uint32_t dataSize);
		void ensureAppendCapacityLocked(size_t appendSize);
		void appendBytesLocked(const uint8_t *data, size_t size);
		void writeHeader(bool syncHeader = false);
		[[nodiscard]] bool validateHeader();

		// Flush write buffer to file (no fsync)
		void flushBuffer();
		// Flush buffer + fsync
		void flushAndSync();
	};

} // namespace graph::storage::wal
