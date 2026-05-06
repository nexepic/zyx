/**
 * @file FileStorage.cpp
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

#include "graph/storage/FileStorage.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <tuple>
#include <utility>
#include "graph/storage/IntegrityChecker.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageBootstrap.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Database.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace graph::storage {

	FileStorage::FileStorage(std::string path, size_t cacheSize, OpenMode mode) :
		dbFilePath(std::move(path)), fileStream(nullptr), cacheSize(cacheSize), openMode_(mode) {}

	FileStorage::~FileStorage() { close(); }

	void FileStorage::open() {
		if (isFileOpen)
			return;

		bool fileExists = std::filesystem::exists(dbFilePath);

		// --- Strict Mode Validation ---
		if (openMode_ == OpenMode::OPEN_EXISTING_FILE && !fileExists) {
			throw std::runtime_error("Database file does not exist: " + dbFilePath);
		}
		if (openMode_ == OpenMode::OPEN_CREATE_NEW_FILE && fileExists) {
			throw std::runtime_error("Database file already exists: " + dbFilePath);
		}
		// ------------------------------------

		// Logic for creating NEW database
		if (openMode_ == OpenMode::OPEN_CREATE_NEW_FILE || (openMode_ == OpenMode::OPEN_CREATE_OR_OPEN_FILE && !fileExists)) {
			// Create and open for Read/Write + Truncate (Wipe previous)
			fileStream = std::make_shared<std::fstream>(dbFilePath, std::ios::binary | std::ios::in | std::ios::out |
																			std::ios::trunc);

			if (!*fileStream) {
				throw std::runtime_error("Cannot create database file: " + dbFilePath);
			}

			fileHeaderManager = std::make_shared<FileHeaderManager>(fileStream, fileHeader);
			// This physically writes the header bytes, creating the "empty" DB file.
			fileHeaderManager->initializeFileHeader();
		}
		// Logic for OPENING existing database
		else {
			fileStream = std::make_shared<std::fstream>(dbFilePath, std::ios::binary | std::ios::in | std::ios::out);

			if (!*fileStream) {
				throw std::runtime_error("Cannot open database file: " + dbFilePath);
			}

			fileHeaderManager = std::make_shared<FileHeaderManager>(fileStream, fileHeader);
			fileHeaderManager->validateAndReadHeader();
		}

		// Open native file handles for pwrite/pread-based parallel I/O
		writeFd_ = portable_open_rw(dbFilePath.c_str());
		if (writeFd_ == INVALID_FILE_HANDLE) {
			throw std::runtime_error("Cannot open native write handle for parallel writes: " + dbFilePath);
		}

		readFd_ = portable_open(dbFilePath.c_str(), O_RDONLY);
		if (readFd_ == INVALID_FILE_HANDLE) {
			throw std::runtime_error("Cannot open native read handle for parallel reads: " + dbFilePath);
		}

		// Create unified I/O abstraction (before components that depend on it)
		storageIO_ = std::make_shared<StorageIO>(fileStream, writeFd_, readFd_);

		initializeComponents();

		isFileOpen = true;
	}

	void FileStorage::initializeComponents() {
		fileHeaderManager->extractFileHeaderInfo();

		segmentTracker = std::make_shared<SegmentTracker>(storageIO_, fileHeader);

		// Initialize per-type ID allocators (without chain walk — StorageBootstrap will provide data)
		std::pair<EntityType, int64_t *> typeMaxPairs[] = {
				{EntityType::Node, &fileHeaderManager->getMaxNodeIdRef()},
				{EntityType::Edge, &fileHeaderManager->getMaxEdgeIdRef()},
				{EntityType::Property, &fileHeaderManager->getMaxPropIdRef()},
				{EntityType::Blob, &fileHeaderManager->getMaxBlobIdRef()},
				{EntityType::Index, &fileHeaderManager->getMaxIndexIdRef()},
				{EntityType::State, &fileHeaderManager->getMaxStateIdRef()},
		};
		for (auto &[type, maxIdPtr] : typeMaxPairs) {
			auto alloc = std::make_shared<IDAllocator>(type, segmentTracker, *maxIdPtr);
			alloc->clearPersistedCaches();
			idAllocators_[static_cast<size_t>(type)] = std::move(alloc);
		}

		// Then create the space management components
		segmentAllocator =
				std::make_shared<SegmentAllocator>(storageIO_, segmentTracker, fileHeaderManager);
		auto segmentCompactor =
				std::make_shared<SegmentCompactor>(storageIO_, segmentTracker, segmentAllocator, fileHeaderManager);
		fileTruncator =
				std::make_shared<FileTruncator>(storageIO_, dbFilePath, segmentTracker);
		spaceManager =
				std::make_shared<SpaceManager>(segmentAllocator, segmentCompactor, fileTruncator, segmentTracker);

		// Initialize data manager (pass storageIO for pread-based parallel reads)
		dataManager = std::make_shared<DataManager>(fileStream, cacheSize, fileHeader, idAllocators_, segmentTracker,
													storageIO_);

		// --- Merged segment chain walk via StorageBootstrap ---
		// Walk each chain ONCE, feeding results to both IDAllocator and SegmentIndexManager.
		StorageBootstrap bootstrap(segmentTracker);
		auto segmentIndexManager = dataManager->getSegmentIndexManager();

		// Initialize segment index head pointers (skip chain walk — we'll provide data below)
		segmentIndexManager->initialize(fileHeader.node_segment_head, fileHeader.edge_segment_head,
										fileHeader.property_segment_head, fileHeader.blob_segment_head,
										fileHeader.index_segment_head, fileHeader.state_segment_head,
										/*skipBuild=*/true);

		struct ChainInfo {
			uint64_t head;
			uint32_t entityType;
		};
		const ChainInfo chains[] = {
			{fileHeader.node_segment_head,     Node::typeId},
			{fileHeader.edge_segment_head,     Edge::typeId},
			{fileHeader.property_segment_head, Property::typeId},
			{fileHeader.blob_segment_head,     Blob::typeId},
			{fileHeader.index_segment_head,    Index::typeId},
			{fileHeader.state_segment_head,    State::typeId},
		};

		for (const auto &chain : chains) {
			auto result = bootstrap.scanChain(chain.head);
			idAllocators_[chain.entityType]->initializeFromScan(result.physicalMaxId);
			segmentIndexManager->setSegmentIndex(chain.entityType, std::move(result.segmentIndexEntries));
		}

		// Initialize DataManager components (skip segment index build — already done above)
		dataManager->initialize(/*skipSegmentIndexBuild=*/true);
		dataManager->setDeletionFlagReference(&deleteOperationPerformed);

		// Wire up EntityReferenceUpdater to the SegmentCompactor
		spaceManager->getCompactor()->setEntityReferenceUpdater(dataManager->getEntityReferenceUpdater());

		// Always set up auto-flush callback
		dataManager->setAutoFlushCallback([this]() { this->flush(); });

		systemStateManager = std::make_shared<state::SystemStateManager>(dataManager);

		dataManager->setSystemStateManager(systemStateManager);

		// Create StorageWriter after all dependencies are ready
		storageWriter_ = std::make_shared<StorageWriter>(storageIO_, segmentTracker, segmentAllocator, dataManager,
														 idAllocators_, fileHeader);

		// Create IntegrityChecker after SegmentTracker and StorageIO are ready
		integrityChecker_ = std::make_shared<IntegrityChecker>(segmentTracker, storageIO_);
	}

	void FileStorage::close() {
		if (isFileOpen) {
			flush(); // Ensure any pending changes are written

			// Always flush file header on close, even when no dirty entities exist.
			// allocateId() increments max ID counters in memory without marking
			// entities dirty. If save() skipped flushFileHeader() (no dirty data),
			// those counter advances would be lost on restart.
			fileHeaderManager->flushFileHeader();

			dataManager->clearCache();
			dataManager->closeFileHandles();

			// Truncate free trailing segments while all handles are still open.
			// Pass the native fd so truncation uses portable_ftruncate()
			// instead of the close-reopen pattern (which leaves a leaked handle
			// on Windows and causes "file in use" errors on deletion).
			(void) fileTruncator->truncateFile(writeFd_);

			// Close the native pwrite handle
			if (writeFd_ != INVALID_FILE_HANDLE) {
				portable_close_rw(writeFd_);
				writeFd_ = INVALID_FILE_HANDLE;
			}

			// Close the native pread handle
			if (readFd_ != INVALID_FILE_HANDLE) {
				portable_close(readFd_);
				readFd_ = INVALID_FILE_HANDLE;
			}

			// Release StorageWriter before its dependencies
			storageWriter_.reset();

			// Release IntegrityChecker before its dependencies
			integrityChecker_.reset();

			// Release StorageIO before closing the stream it wraps
			storageIO_.reset();

			// Close the fstream
			if (fileStream) {
				fileStream->flush();
				fileStream->close();
				fileStream.reset();
			}

			dataManager.reset();
			isFileOpen = false;
		}
	}

	void FileStorage::save() {
		using Clock = std::chrono::steady_clock;

		if (!isFileOpen)
			throw std::runtime_error("Database must be open before saving");
		if (!dataManager->hasUnsavedChanges())
			return;

		auto totalStart = Clock::now();

		// 1. ATOMIC SNAPSHOT: Freeze current dirty state into snapshot
		auto snapshot = dataManager->prepareFlushSnapshot();

		if (snapshot.isEmpty())
			return;

		// 2. I/O PHASE: Classify + write all entity types via StorageWriter
		auto ioStart = Clock::now();

		storageWriter_->writeSnapshot(snapshot, threadPool_);

		// 3. Persist segment headers (so pread-based reads see correct used/start_id)
		persistSegmentHeaders();

		// Update aggregated CRC from segment CRCs before flushing file header
		auto segmentCrcs = segmentTracker->collectSegmentCrcs();
		fileHeaderManager->updateAggregatedCrc(segmentCrcs);

		fileHeaderManager->flushFileHeader();

		// 4. Single fsync to flush all writes (entity data + segment headers)
		storageIO_->sync();

		debug::PerfTrace::addDuration(
				"save.io", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													ioStart)
												.count()));

		// 5. COMMIT: Clear the snapshot data and invalidate stale cached pages
		dataManager->commitFlushSnapshot();
		dataManager->invalidateDirtySegments(snapshot);
		debug::PerfTrace::addDuration(
				"save.total", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													  totalStart)
													.count()));
	}

	template<typename T>
	void FileStorage::saveData(std::vector<T> &data, uint64_t &segmentHead, uint32_t maxSegmentSize) {
		storageWriter_->saveData(data, segmentHead, maxSegmentSize);
	}

	uint64_t FileStorage::allocateSegment(uint32_t type, uint32_t capacity) const {
		return segmentAllocator->allocateSegment(type, capacity);
	}

	template<typename T>
	void FileStorage::updateEntityInPlace(const T &entity, uint64_t knownSegmentOffset) {
		storageWriter_->updateEntityInPlace(entity, knownSegmentOffset);
	}

	template<typename T>
	void FileStorage::deleteEntityOnDisk(const T &entity) {
		storageWriter_->deleteEntityOnDisk(entity);
	}

	// Template instantiations for delegate methods
	template void FileStorage::saveData<Node>(std::vector<Node> &, uint64_t &, uint32_t);
	template void FileStorage::saveData<Edge>(std::vector<Edge> &, uint64_t &, uint32_t);
	template void FileStorage::saveData<Property>(std::vector<Property> &, uint64_t &, uint32_t);
	template void FileStorage::saveData<Blob>(std::vector<Blob> &, uint64_t &, uint32_t);
	template void FileStorage::saveData<Index>(std::vector<Index> &, uint64_t &, uint32_t);
	template void FileStorage::saveData<State>(std::vector<State> &, uint64_t &, uint32_t);

	template void FileStorage::updateEntityInPlace<Node>(const Node &, uint64_t);
	template void FileStorage::updateEntityInPlace<Edge>(const Edge &, uint64_t);
	template void FileStorage::updateEntityInPlace<Property>(const Property &, uint64_t);
	template void FileStorage::updateEntityInPlace<Blob>(const Blob &, uint64_t);
	template void FileStorage::updateEntityInPlace<Index>(const Index &, uint64_t);
	template void FileStorage::updateEntityInPlace<State>(const State &, uint64_t);

	template void FileStorage::deleteEntityOnDisk<Node>(const Node &);
	template void FileStorage::deleteEntityOnDisk<Edge>(const Edge &);

	bool FileStorage::verifyBitmapConsistency(uint64_t segmentOffset) const {
		return integrityChecker_->verifyBitmapConsistency(segmentOffset);
	}

	void FileStorage::persistSegmentHeaders() const {
		// Simply delegate to the SegmentTracker to flush all dirty segments
		segmentTracker->flushDirtySegments();
	}

	void FileStorage::registerEventListener(std::weak_ptr<IStorageEventListener> listener) {
		std::lock_guard<std::mutex> lock(listenerMutex_);
		eventListeners_.push_back(std::move(listener));
	}

	// TODO: Subsequent flush operations should be executed in a queue
	void FileStorage::flush() {
		// Acquire lock to ensure atomic operation
		std::unique_lock<std::mutex> lock(flushMutex, std::try_to_lock);

		// If we couldn't acquire the lock or a flush is already in progress, return
		if (!lock.owns_lock() || flushInProgress.exchange(true)) {
			return;
		}

		try {
			// --- NOTIFY LISTENERS ---
			{
				std::lock_guard<std::mutex> listenerLock(listenerMutex_);
				// Iterate and notify valid listeners
				auto it = eventListeners_.begin();
				while (it != eventListeners_.end()) {
					if (auto listener = it->lock()) {
						// Notify IndexManager or others to persist their state
						listener->onStorageFlush();
						++it;
					} else {
						// Remove expired listeners (e.g., if IndexManager was destroyed)
						it = eventListeners_.erase(it);
					}
				}
			}

			// Ensure all pending changes are written
			save();

			// Only check for compaction if delete operations occurred
			if (deleteOperationPerformed.load() && compactionEnabled_.load()) {
				if (spaceManager->shouldCompact()) {
					// Use the thread-safe compaction method

					// Only perform post-compaction operations if compaction was successful
					if (spaceManager->safeCompactSegments()) {
						dataManager->clearCache();

						// Just clear all allocator caches. They will lazy-load on next insert.
						for (auto &alloc : idAllocators_) alloc->resetAfterCompaction();

						dataManager->getSegmentIndexManager()->buildSegmentIndexes();

						// Re-persist headers after compaction modified segments
						persistSegmentHeaders();
						auto compactSegCrcs = segmentTracker->collectSegmentCrcs();
						fileHeaderManager->updateAggregatedCrc(compactSegCrcs);
						fileHeaderManager->flushFileHeader();
					}
				}
				// Reset the flag after handling potential compaction
				deleteOperationPerformed.store(false);
			}
		} catch (const std::exception &e) {
			// Log the error
			std::cerr << "Exception during flush operation: " << e.what() << std::endl;
		} catch (...) {
			// Log unknown errors
			std::cerr << "Unknown exception during flush operation" << std::endl;
		}

		// Mark flush as complete
		flushInProgress.store(false);
	}

	void FileStorage::clearCache() const { dataManager->clearCache(); }

	FileStorage::IntegrityResult FileStorage::verifyIntegrity() const {
		return integrityChecker_->verifyIntegrity();
	}

} // namespace graph::storage
