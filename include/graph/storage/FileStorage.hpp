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
#include "graph/storage/IntegrityChecker.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageWriter.hpp"
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

		[[nodiscard]] std::shared_ptr<StorageIO> getStorageIO() const { return storageIO_; }

		[[nodiscard]] std::shared_ptr<IntegrityChecker> getIntegrityChecker() const { return integrityChecker_; }

		[[nodiscard]] std::shared_ptr<DataManager> getDataManager() const { return dataManager; }

		[[nodiscard]] const IDAllocators &getIDAllocators() const { return idAllocators_; }
		[[nodiscard]] std::shared_ptr<IDAllocator> getIDAllocator(EntityType type) const {
			return idAllocators_[static_cast<size_t>(type)];
		}
		[[nodiscard]] std::shared_ptr<IDAllocator> getIDAllocator(uint32_t typeId) const {
			return idAllocators_[typeId];
		}

		[[nodiscard]] std::shared_ptr<StorageWriter> getStorageWriter() const { return storageWriter_; }

		[[nodiscard]] std::shared_ptr<FileHeaderManager> getFileHeaderManager() const { return fileHeaderManager; }

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

		using SegmentVerifyResult = IntegrityChecker::SegmentVerifyResult;
		using IntegrityResult = IntegrityChecker::IntegrityResult;

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

		IDAllocators idAllocators_;

		std::shared_ptr<FileHeaderManager> fileHeaderManager;

		std::shared_ptr<DataManager> dataManager;

		std::shared_ptr<SegmentAllocator> segmentAllocator;
		std::shared_ptr<SpaceManager> spaceManager;
		std::shared_ptr<FileTruncator> fileTruncator;

		std::shared_ptr<SegmentTracker> segmentTracker;

		std::shared_ptr<state::SystemStateManager> systemStateManager;

		std::vector<std::weak_ptr<IStorageEventListener>> eventListeners_;
		std::mutex listenerMutex_;

		std::mutex flushMutex;
		std::atomic<bool> flushInProgress{false};
		std::atomic<bool> deleteOperationPerformed{false};

		std::atomic<bool> compactionEnabled_{false};
		concurrent::ThreadPool *threadPool_ = nullptr;

		// Native file handle for pwrite-based parallel writes and truncation.
		// Opened alongside fstream in open(), closed in close().
		file_handle_t writeFd_ = INVALID_FILE_HANDLE;

		// Native file handle for pread-based parallel reads.
		// Opened alongside fstream in open(), closed in close().
		file_handle_t readFd_ = INVALID_FILE_HANDLE;

		// Unified I/O abstraction wrapping fstream + writeFd + readFd
		std::shared_ptr<StorageIO> storageIO_;

		// Entity write engine (saveData, updateEntityInPlace, deleteEntityOnDisk, etc.)
		std::shared_ptr<StorageWriter> storageWriter_;

		// Diagnostic verification engine (verifyBitmapConsistency, verifyIntegrity)
		std::shared_ptr<IntegrityChecker> integrityChecker_;
	};
} // namespace graph::storage
