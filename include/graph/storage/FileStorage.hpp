/**
 * @file FileStorage.hpp
 * @author Nexepic
 * @date 2025/2/26
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
#include <string>
#include <tuple>
#include <unordered_map>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "DatabaseInspector.hpp"
#include "DeletionManager.hpp"
#include "FileHeaderManager.hpp"
#include "FileTruncator.hpp"
#include "IDAllocator.hpp"
#include "IStorageEventListener.hpp"
#include "SegmentAllocator.hpp"
#include "SpaceManager.hpp"
#include "StorageHeaders.hpp"
#include "StorageTypes.hpp"
#include "data/DataManager.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "state/SystemStateManager.hpp"

namespace graph::storage {

	class FileStorage {
	public:
		FileStorage(std::string path, size_t cacheSize, OpenMode mode = OpenMode::OPEN_CREATE_OR_OPEN_FILE);
		~FileStorage();

		void open();
		void save();
		void close();
		void flush();

		void initializeComponents();

		void persistSegmentHeaders() const;

		template<typename T>
		void saveData(std::vector<T> &data, uint64_t &segmentHead, uint32_t maxSegmentSize);

		template<typename T>
		void writeSegmentData(uint64_t segmentOffset, const std::vector<T> &data, uint32_t usedItems);

		uint64_t allocateSegment(uint32_t type, uint32_t capacity) const;


		// Cache management
		void clearCache() const;

		template<typename T>
		void updateEntityInPlace(const T &entity, uint64_t knownSegmentOffset = 0);

		template<typename T>
		void deleteEntityOnDisk(const T &entity);

		template<typename T>
		void saveNewEntities(std::vector<T> &entities);

		template<typename T>
		void saveModifiedAndDeleted(const std::vector<T> &modified, const std::vector<T> &deleted);

		// isFileOpen getter
		[[nodiscard]] bool isOpen() const { return isFileOpen; }

		// Verify bitmap consistency for debugging purposes
		bool verifyBitmapConsistency(uint64_t segmentOffset) const;

		[[nodiscard]] std::shared_ptr<SegmentTracker> getSegmentTracker() const { return segmentTracker; }

		[[nodiscard]] std::shared_ptr<DataManager> getDataManager() const { return dataManager; }

		[[nodiscard]] std::shared_ptr<IDAllocator> getIDAllocator() const { return idAllocator; }

		[[nodiscard]] std::shared_ptr<state::SystemStateManager> getSystemStateManager() const {
			return systemStateManager;
		}

		void registerEventListener(std::weak_ptr<IStorageEventListener> listener);

		std::shared_ptr<DatabaseInspector> getInspector() const {
			return std::make_shared<DatabaseInspector>(fileHeader, fileStream, *dataManager);
		}

		void setCompactionEnabled(bool enabled) { compactionEnabled_.store(enabled); }

		bool isCompactionEnabled() const { return compactionEnabled_.load(); }

		void setThreadPool(concurrent::ThreadPool *pool) { threadPool_ = pool; }

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

		[[nodiscard]] IntegrityResult verifyIntegrity() const;

	private:
		std::string dbFilePath;

		FileHeader fileHeader;

		bool isFileOpen = false;

		// Single file stream for all operations
		std::shared_ptr<std::fstream> fileStream;

		// cacheSize
		size_t cacheSize;

		OpenMode openMode_;

		std::shared_ptr<IDAllocator> idAllocator;

		std::shared_ptr<FileHeaderManager> fileHeaderManager;

		std::shared_ptr<DataManager> dataManager;

		std::shared_ptr<SegmentAllocator> segmentAllocator;
		std::shared_ptr<SpaceManager> spaceManager;
		std::shared_ptr<FileTruncator> fileTruncator;

		std::shared_ptr<DatabaseInspector> databaseInspector;

		std::shared_ptr<SegmentTracker> segmentTracker;

		std::shared_ptr<state::SystemStateManager> systemStateManager;

		std::vector<std::weak_ptr<IStorageEventListener>> eventListeners_;
		std::mutex listenerMutex_;

		// Update bitmap for an entity in the segment header
		template<typename EntityType>
		void updateBitmapForEntity(uint64_t segmentOffset, uint64_t entityId, bool isActive);

		// Update bitmap when writing segment data in batch
		void updateSegmentBitmap(uint64_t segmentOffset, uint64_t startId, uint32_t count, bool isActive = true) const;

		// Read the current segment header from disk
		SegmentHeader readSegmentHeader(uint64_t segmentOffset) const;

		// Write updated segment header to disk
		void writeSegmentHeader(uint64_t segmentOffset, const SegmentHeader &header) const;

		std::mutex flushMutex;
		std::atomic<bool> flushInProgress{false};
		std::atomic<bool> deleteOperationPerformed{false};

		std::atomic<bool> compactionEnabled_{false};
		concurrent::ThreadPool *threadPool_ = nullptr;

		// Native file handle for pwrite-based parallel writes and truncation.
		// Opened alongside fstream in open(), closed in close().
		file_handle_t writeFd_ = INVALID_FILE_HANDLE;
	};
} // namespace graph::storage
