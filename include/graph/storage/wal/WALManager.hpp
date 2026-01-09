/**
 * @file WALManager.hpp
 * @author Nexepic
 * @date 2025/3/24
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include "WALRecordImpl.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"

namespace graph::storage::wal {

	struct WALConfig {
		// Path to WAL directory
		std::string walPath = "./wal";

		// Maximum WAL file size in bytes before rotation
		size_t maxFileSize = 64 * 1024 * 1024; // 64MB

		// How often to automatically flush WAL buffers (ms)
		unsigned flushInterval = 200; // 200ms

		// Maximum number of WAL files to keep
		unsigned maxWalFiles = 10;

		// Background checkpoint interval (ms)
		unsigned checkpointInterval = 30000; // 30s

		// Write batch size before forced flush
		unsigned writeBatchSize = 1000;
	};

	class WALManager {
	public:
		explicit WALManager(const WALConfig &config);
		~WALManager();

		// Initialize the WAL system
		void initialize();

		// Shutdown the WAL system gracefully
		void shutdown();

		// Log operations (returns LSN - Log Sequence Number)
		uint64_t logNodeInsert(const graph::Node &node);
		uint64_t logNodeUpdate(const graph::Node &node);
		uint64_t logNodeDelete(uint64_t nodeId);

		uint64_t logEdgeInsert(const graph::Edge &edge);
		uint64_t logEdgeUpdate(const graph::Edge &edge);
		uint64_t logEdgeDelete(uint64_t edgeId);

		// Transaction support
		uint64_t beginTransaction();
		void commitTransaction(uint64_t txnId);
		void rollbackTransaction(uint64_t txnId);

		// Force WAL flush to disk
		void flush();

		// Create a checkpoint (applies WAL to main DB)
		uint64_t checkpoint();

		// Recovery functions
		bool needsRecovery() const;
		void performRecovery(std::function<void(const WALRecord *)> applyFunc);

		// Get current LSN
		uint64_t getCurrentLSN() const { return currentLSN_.load(); }

	private:
		// Configuration
		WALConfig config_;

		// Current log file
		std::string currentLogPath_;
		std::ofstream logFile_;

		// Current position and sequence
		std::atomic<uint64_t> currentLSN_{1}; // Log Sequence Number
		std::atomic<uint64_t> currentOffset_{0}; // Offset in current file
		std::atomic<uint32_t> fileCounter_{0}; // Current WAL file number

		// Last checkpoint information
		std::atomic<uint64_t> lastCheckpointLSN_{0};

		// Thread safety
		std::mutex walMutex_;

		// Background flusher
		std::atomic<bool> running_{false};
		std::thread flushThread_;
		std::thread checkpointThread_;
		std::condition_variable flushCondition_;

		// Pending writes queue
		std::queue<std::unique_ptr<WALRecord>> pendingWrites_;
		std::mutex queueMutex_;

		// Generate a new WAL file
		void rotateLogFileIfNeeded();
		std::string generateLogFileName(uint32_t fileNum);

		// Background flush thread function
		void backgroundFlushThread();

		// Background checkpoint thread function
		void backgroundCheckpointThread();

		// Internal write operation
		uint64_t writeRecord(std::unique_ptr<WALRecord> record);

		// List all WAL files in sequence
		std::vector<std::string> getWalFiles() const;

		// Clean up old WAL files
		void cleanupOldWalFiles();

		// Transaction ID generator
		std::atomic<uint64_t> nextTxnId_{1};
	};

} // namespace graph::storage::wal
