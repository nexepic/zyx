/**
 * @file RelocatableStorageBackend.cpp
 * @brief File-owning implementation of the relocatable logical read backend.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifndef _WIN32
	#include <sys/file.h>
#endif

#include "graph/common/VectorMath.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageFormat.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeMembershipAccessCost.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeEndpointCache.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgePublicationPreflight.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentRunBuilder.hpp"
#include "graph/storage/relocatable/entity/PageBackedConstraintMutationAdapter.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/ExternalBTreeEntrySorter.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"
#include "graph/storage/relocatable/index/ScalarIndexTreeReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorBaseRebuild.hpp"
#include "graph/storage/relocatable/maintenance/VectorIndexStateReclaimer.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/storage/relocatable/schema/ConstraintMutationValidator.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/schema/NativeConstraintDefinitionWriter.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/RelocatableVectorSearch.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorMappingIds.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] std::string nativeBulkSessionFileIdentity(file_handle_t handle) {
			if (handle == INVALID_FILE_HANDLE) { // the only caller obtains the handle from an open writable backend after verifying its data file.
				throw std::logic_error("relocatable bulk session requires a native writable file handle");
			}
#ifdef _WIN32
			BY_HANDLE_FILE_INFORMATION information = {};
			if (!::GetFileInformationByHandle(reinterpret_cast<HANDLE>(handle), &information)) {
				throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
									"relocatable bulk session could not identify the database file");
			}
			const uint64_t fileIndex = (static_cast<uint64_t>(information.nFileIndexHigh) << 32U) |
								   static_cast<uint64_t>(information.nFileIndexLow);
			return std::to_string(information.dwVolumeSerialNumber) + ":" + std::to_string(fileIndex);
#else
			struct stat information = {};
			if (::fstat(handle, &information) != 0) {
				throw std::system_error(errno, std::generic_category(),
									"relocatable bulk session could not identify the database file");
			}
			return std::to_string(static_cast<uintmax_t>(information.st_dev)) + ":" +
				   std::to_string(static_cast<uintmax_t>(information.st_ino));
#endif
		}

		[[nodiscard]] std::shared_ptr<std::mutex>
		nativeBulkSessionProcessMutex(file_handle_t handle) {
			static std::mutex registryMutex;
			static std::map<std::string, std::weak_ptr<std::mutex>> registry;
			// Key by the opened file identity so hard-link and symlink aliases share a lease.
			const std::string key = nativeBulkSessionFileIdentity(handle);
			std::lock_guard lock(registryMutex);
			for (auto position = registry.begin(); position != registry.end();) {
				if (position->second.expired()) {
					position = registry.erase(position);
				} else {
					++position;
				}
			}
			if (const auto existing = registry[key].lock(); existing != nullptr) {
				return existing;
			}
			auto created = std::make_shared<std::mutex>();
			registry[key] = created;
			return created;
		}

		[[nodiscard]] bool tryLockNativeBulkSessionFile(file_handle_t handle) {
			if (handle == INVALID_FILE_HANDLE) { // the native session lease is created only from the validated writable backend handle.
				throw std::logic_error("relocatable bulk session requires a native writable file handle");
			}
#ifdef _WIN32
			OVERLAPPED overlapped = {};
			if (::LockFileEx(reinterpret_cast<HANDLE>(handle),
							 LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U, &overlapped)) {
				return true;
			}
			if (::GetLastError() == ERROR_LOCK_VIOLATION) {
				return false;
			}
			throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
								"relocatable bulk session could not lock the database");
#else
			// Unlike process-scoped fcntl record locks, flock stays attached to this
			// open handle when another backend in the same process closes its own FD.
			if (::flock(handle, LOCK_EX | LOCK_NB) == 0) {
				return true;
			}
			if (errno == EACCES || errno == EAGAIN) {
				return false;
			}
			throw std::system_error(errno, std::generic_category(),
								"relocatable bulk session could not lock the database");
#endif
		}

		void unlockNativeBulkSessionFile(file_handle_t handle) noexcept {
			if (handle == INVALID_FILE_HANDLE) {
				return;
			}
#ifdef _WIN32
			OVERLAPPED overlapped = {};
			static_cast<void>(::UnlockFileEx(reinterpret_cast<HANDLE>(handle), 0U, 1U, 0U, &overlapped));
#else
			static_cast<void>(::flock(handle, LOCK_UN));
#endif
		}

	} // namespace

	class NativeBulkSessionLease final {
	public:
		NativeBulkSessionLease(std::shared_ptr<std::mutex> processMutex, file_handle_t handle) :
				process_mutex_(std::move(processMutex)), process_lock_(*process_mutex_, std::try_to_lock) {
			if (!process_lock_.owns_lock()) {
				return;
			}
			if (tryLockNativeBulkSessionFile(handle)) {
				handle_ = handle;
			}
		}

		~NativeBulkSessionLease() { unlockNativeBulkSessionFile(handle_); }

		NativeBulkSessionLease(const NativeBulkSessionLease &) = delete;
		NativeBulkSessionLease &operator=(const NativeBulkSessionLease &) = delete;

		[[nodiscard]] bool isHeld() const noexcept { return handle_ != INVALID_FILE_HANDLE; }

	private:
		std::shared_ptr<std::mutex> process_mutex_;
		std::unique_lock<std::mutex> process_lock_;
		file_handle_t handle_ = INVALID_FILE_HANDLE;
	};

	class OwnedRelocatableBackendFile final {
	public:
		[[nodiscard]] static std::unique_ptr<OwnedRelocatableBackendFile> create(const std::filesystem::path &path,
																				 size_t bufferPoolBytes) {
			auto result = std::unique_ptr<OwnedRelocatableBackendFile>(new OwnedRelocatableBackendFile());
			result->write_fd_ = portable_create_rw_exclusive(path.string().c_str());
			if (result->write_fd_ == INVALID_FILE_HANDLE) {
				throw std::runtime_error("relocatable backend cannot exclusively create the database file");
			}
			result->stream_ = std::make_shared<std::fstream>(path, std::ios::binary | std::ios::in | std::ios::out);
			if (!result->stream_->is_open()) {
				throw std::runtime_error("relocatable backend cannot open the newly created database file");
			}
			result->read_fd_ = portable_open(path.string().c_str(), O_RDONLY);
			result->io_ = std::make_shared<StorageIO>(result->stream_, result->write_fd_, result->read_fd_,
													  StorageIOAccessMode::SIOAM_READ_WRITE);
			result->io_->enableBufferPool(bufferPoolBytes);
			return result;
		}

		[[nodiscard]] static std::unique_ptr<OwnedRelocatableBackendFile> open(const std::filesystem::path &path,
																			   bool writable, size_t bufferPoolBytes) {
			auto result = std::unique_ptr<OwnedRelocatableBackendFile>(new OwnedRelocatableBackendFile());
			const std::ios::openmode mode =
					std::ios::binary | std::ios::in | (writable ? std::ios::out : std::ios::openmode{});
			result->stream_ = std::make_shared<std::fstream>(path, mode);
			if (!result->stream_->is_open()) {
				throw std::runtime_error("relocatable backend cannot open the existing database file");
			}
			if (writable) {
				result->write_fd_ = portable_open_rw(path.string().c_str());
			}
			result->read_fd_ = portable_open(path.string().c_str(), O_RDONLY);
			// StorageIO falls back to its serialized fstream path if an optional
			// native handle is unavailable on this platform or filesystem.
			result->io_ = std::make_shared<StorageIO>(result->stream_, result->write_fd_, result->read_fd_,
													  writable ? StorageIOAccessMode::SIOAM_READ_WRITE
															   : StorageIOAccessMode::SIOAM_READ_ONLY);
			result->io_->enableBufferPool(bufferPoolBytes);
			return result;
		}

		~OwnedRelocatableBackendFile() {
			io_.reset();
			static_cast<void>(portable_close_rw(write_fd_));
			static_cast<void>(portable_close(read_fd_));
			stream_.reset();
		}

		[[nodiscard]] const std::shared_ptr<StorageIO> &io() const { return io_; }

	private:
		OwnedRelocatableBackendFile() = default;

		std::shared_ptr<std::fstream> stream_;
		std::shared_ptr<StorageIO> io_;
		file_handle_t write_fd_ = INVALID_FILE_HANDLE;
		file_handle_t read_fd_ = INVALID_FILE_HANDLE;
	};

	namespace {

		constexpr uint64_t RSS_LOGICAL_GRAPH_FENCE_FORMAT_TOKEN = UINT64_C(0x564E4558544C4746);
		using LogicalGraphFenceRootMember = PhysicalPageRef Superblock::*;
		constexpr std::array<LogicalGraphFenceRootMember, LogicalGraphMutationFence::GRAPH_STATE_ROOT_COUNT>
				RSS_LOGICAL_GRAPH_FENCE_ROOT_MEMBERS = {
						&Superblock::node_entity_directory_root,	 &Superblock::edge_entity_directory_root,
						&Superblock::entity_property_directory_root, &Superblock::large_value_manifest_directory_root,
						&Superblock::vector_location_directory_root, &Superblock::vector_manifest_directory_root,
						&Superblock::state_directory_root,
		};

		[[noreturn]] void failVectorTraining(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		void traceNativeBulkCandidateMetric(std::string_view key, size_t value) {
			if (value > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("relocatable native bulk candidate metric exceeds PerfTrace range");
			}
			debug::PerfTrace::addValue(key, static_cast<int64_t>(value));
		}

		void traceNativeCandidateMetric(std::string_view key, size_t value) {
			if (value > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("relocatable native index candidate metric exceeds PerfTrace range");
			}
			debug::PerfTrace::addValue(key, static_cast<int64_t>(value));
		}

		[[nodiscard]] size_t checkedBoundPlusOne(size_t bound, const char *message) {
			if (bound == std::numeric_limits<size_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			return bound + 1U;
		}

		[[nodiscard]] size_t candidateBatchBytes(const IndexCandidateBatch &batch) {
			if (batch.candidates.capacity() > std::numeric_limits<size_t>::max() / sizeof(EntityRef)) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
						"relocatable native index candidate byte accounting overflowed");
			}
			size_t bytes = batch.candidates.capacity() * sizeof(EntityRef);
			for (const IndexCandidateBatch::VerifiedPropertyColumn &column: batch.verified_property_columns) {
				if (column.key.size() > std::numeric_limits<size_t>::max() - bytes) {
					throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
							"relocatable native index candidate byte accounting overflowed");
				}
				bytes += column.key.size();
				if (column.values.capacity() > (std::numeric_limits<size_t>::max() - bytes) / sizeof(PropertyValue)) {
					throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
							"relocatable native index candidate byte accounting overflowed");
				}
				bytes += column.values.capacity() * sizeof(PropertyValue);
				for (const PropertyValue &value: column.values) {
					const size_t payload = property_utils::getPropertyValueSize(value);
					if (payload > std::numeric_limits<size_t>::max() - bytes) {
						throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable native index candidate byte accounting overflowed");
					}
					bytes += payload;
				}
			}
			return bytes;
		}

		void enforceCandidateBatchBytes(const IndexCandidateBatch &batch, size_t maximumBytes) {
			if (maximumBytes == 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
						"relocatable native index candidate stream requires a positive byte bound");
			}
			if (maximumBytes != std::numeric_limits<size_t>::max() && candidateBatchBytes(batch) > maximumBytes) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable native index candidate batch exceeded its byte bound");
			}
		}

		class CandidateDedupMemoryResource final : public std::pmr::memory_resource {
		public:
			explicit CandidateDedupMemoryResource(size_t maximumBytes): maximum_bytes_(maximumBytes) {}
			[[nodiscard]] size_t retainedBytes() const noexcept { return retained_bytes_; }
			void setAllocationAdmission(std::function<void(size_t)> admission) { allocation_admission_ = std::move(admission); }
		private:
			void *do_allocate(size_t bytes, size_t alignment) override {
				if (allocation_admission_) allocation_admission_(bytes);
				if (bytes > maximum_bytes_ - retained_bytes_) {
					traceNativeCandidateMetric("native_graph.read.range_dedup_allocation_rejected", 1U);
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable native range candidate deduplication exceeded its byte bound");
				}
				void *result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
				retained_bytes_ += bytes;
				return result;
			}
			void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
				std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
				retained_bytes_ -= bytes;
			}
			bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
				return this == &other;
			}
			std::function<void(size_t)> allocation_admission_;
			size_t maximum_bytes_;
			size_t retained_bytes_ = 0U;
		};

		void reserveCandidateOutput(IndexCandidateBatch &batch, size_t rows,
				std::span<const std::string> keys, const SelectedPropertyReadAdmission &admit) {
			size_t retained = 0U;
			const auto check = [&](size_t count, size_t width) {
				if (count > (std::numeric_limits<size_t>::max() - retained) / width) {
					throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "candidate output capacity overflowed");
				}
				admit(retained + count * width);
			};
			check(rows, sizeof(EntityRef));
			batch.candidates.reserve(rows);
			check(batch.candidates.capacity(), sizeof(EntityRef));
			retained += batch.candidates.capacity() * sizeof(EntityRef);
			check(keys.size(), sizeof(IndexCandidateBatch::VerifiedPropertyColumn));
			batch.verified_property_columns.reserve(keys.size());
			check(batch.verified_property_columns.capacity(), sizeof(IndexCandidateBatch::VerifiedPropertyColumn));
			retained += batch.verified_property_columns.capacity() * sizeof(IndexCandidateBatch::VerifiedPropertyColumn);
			for (const auto &key: keys) {
				check(key.size(), sizeof(char));
				batch.verified_property_columns.push_back({key, {}});
				auto &column = batch.verified_property_columns.back();
				check(column.key.capacity(), sizeof(char));
				retained += column.key.capacity();
				check(rows, sizeof(PropertyValue));
				column.values.reserve(rows);
				check(column.values.capacity(), sizeof(PropertyValue));
				retained += column.values.capacity() * sizeof(PropertyValue);
			}
		}

		void reorderCandidateRowsInPlace(IndexCandidateBatch &batch, std::span<size_t> order) {
			for (const auto &column: batch.verified_property_columns) {
				if (column.values.size() != order.size()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable candidate reorder found an invalid Property column");
				}
			}
			// Rotate destination-to-source cycles across candidates and all columns.
			// The existing permutation also records which cycles are consumed.
			for (size_t start = 0U; start < order.size(); ++start) {
				size_t current = start;
				while (order[current] != start) {
					const size_t next = order[current];
					std::swap(batch.candidates[current], batch.candidates[next]);
					for (auto &column: batch.verified_property_columns) {
						std::swap(column.values[current], column.values[next]);
					}
					order[current] = current;
					current = next;
				}
				order[current] = current;
			}
		}

		[[nodiscard]] size_t candidateBatchCapacity(size_t maximumNodes, size_t maximumBytes) {
			// Keep producer staging below the caller's byte budget. The authenticated
			// batch-size check remains authoritative because Property payloads are
			// variable-sized; this estimate only controls how many candidates are
			// staged before that check.
			size_t capacity = std::min(BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES, maximumNodes);
			constexpr size_t kConservativeCandidateBytes = 8U * 1024U;
			if (maximumBytes != std::numeric_limits<size_t>::max()) {
				const size_t byteCapacity = maximumBytes / kConservativeCandidateBytes;
				capacity = std::min(capacity, std::max<size_t>(1U, byteCapacity));
			}
			return std::max<size_t>(1U, capacity);
		}

		// Exact predicates supply the values that will be copied into verified
		// columns. Check their known retained sizes before reserving/copying cells.
		[[nodiscard]] size_t exactCandidateRowBytes(std::span<const PropertyValue> values) {
			size_t bytes = sizeof(EntityRef);
			for (const auto &value: values) {
				const size_t payload = property_utils::getPropertyValueSize(value);
				if (payload > std::numeric_limits<size_t>::max() - sizeof(PropertyValue) ||
					payload + sizeof(PropertyValue) > std::numeric_limits<size_t>::max() - bytes) {
					throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable native index candidate byte accounting overflowed");
				}
				bytes += sizeof(PropertyValue) + payload;
			}
			return bytes;
		}

		void prepareExactCandidateColumns(IndexCandidateBatch &batch, size_t count,
				std::span<const PropertyValue> values, size_t maximumBytes) {
			if (maximumBytes == 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER, "relocatable native index candidate stream requires a positive byte bound");
			}
			const size_t rowBytes = exactCandidateRowBytes(values);
			size_t keyBytes = 0U;
			for (const auto &column: batch.verified_property_columns) {
				if (column.key.capacity() > std::numeric_limits<size_t>::max() - keyBytes) {
					throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable native index candidate byte accounting overflowed");
				}
				keyBytes += column.key.capacity();
			}
			if (keyBytes > maximumBytes || count > (maximumBytes - keyBytes) / rowBytes) {
				throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable native index candidate batch exceeded its byte bound");
			}
			batch.candidates.reserve(count);
			for (auto &column: batch.verified_property_columns) column.values.reserve(count);
			// Account for the actual capacities chosen by the standard library too.
			const size_t baseBytes = candidateBatchBytes(batch);
			const size_t payloadPerRow = rowBytes - sizeof(EntityRef) - values.size() * sizeof(PropertyValue);
			if (baseBytes > maximumBytes || (payloadPerRow != 0U && count > (maximumBytes - baseBytes) / payloadPerRow)) {
				throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable native index candidate batch exceeded its byte bound");
			}
		}

		void checkExactCandidateSourceBytes(size_t used, size_t buffers, size_t maximumBytes) {
			if (used > maximumBytes || buffers > maximumBytes - used)
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable native index candidate batch exceeded its byte bound (retained=" +
						std::to_string(used) + ", scratch=" + std::to_string(buffers) +
						", limit=" + std::to_string(maximumBytes) + ")");
		}
		[[nodiscard]] SelectedPropertyReadAdmission exactCandidateSourceAdmission(
				size_t &used, size_t &buffers, size_t maximumBytes) {
			return [&used, &buffers, maximumBytes](size_t bytes) {
				checkExactCandidateSourceBytes(used, bytes, maximumBytes);
				checkExactCandidateSourceBytes(used + bytes, buffers, maximumBytes);
				used += bytes;
				traceNativeCandidateMetric("native_graph.read.exact_candidate_source_bytes", bytes);
			};
		}
		[[nodiscard]] SelectedPropertyReadAdmission exactCandidateBufferAdmission(
				size_t &used, size_t &buffers, size_t maximumBytes) {
			return [&used, &buffers, maximumBytes](size_t bytes) {
				checkExactCandidateSourceBytes(used, bytes, maximumBytes);
				buffers = bytes;
			};
		}

		void checkVectorTrainingCancellation(const LogicalGraphVectorTrainingRequest &request) {
			if (request.cancellation != nullptr) {
				request.cancellation->check();
			}
		}

		class PreparedRelocatableVectorTraining final : public PreparedLogicalGraphVectorTraining {
		public:
			PreparedRelocatableVectorTraining(std::unique_lock<std::mutex> publicationLock,
											  LogicalGraphVectorTrainingPreview preview,
											  std::function<LogicalGraphVectorTrainingResult()> commit) :
				publication_lock_(std::move(publicationLock)), preview_(preview), commit_(std::move(commit)) {}

			[[nodiscard]] LogicalGraphVectorTrainingPreview preview() const noexcept override { return preview_; }

			[[nodiscard]] LogicalGraphVectorTrainingResult commit() override {
				if (committed_) {
					throw std::logic_error("prepared relocatable Vector training was committed more than once");
				}
				LogicalGraphVectorTrainingResult result = commit_();
				if (result.trained != preview_.will_train) {
					throw std::logic_error("prepared relocatable Vector training changed its published status");
				}
				committed_ = true;
				return result;
			}

		private:
			std::unique_lock<std::mutex> publication_lock_;
			LogicalGraphVectorTrainingPreview preview_;
			std::function<LogicalGraphVectorTrainingResult()> commit_;
			bool committed_ = false;
		};

		[[nodiscard]] LogSequenceNumber nextVectorTrainingCheckpoint(const RelocatableMetadataStore &metadata) {
			MetadataSnapshotPin snapshot = metadata.openSnapshot();
			const uint64_t current = snapshot.superblock().checkpoint_lsn.value();
			snapshot.release();
			if (current == std::numeric_limits<uint64_t>::max()) { // backend open rejects an exhausted checkpoint during scratch recovery before any public Vector-training entry point can call this private helper.
				failVectorTraining(FormatError::FERR_ARITHMETIC_OVERFLOW,
								   "relocatable Vector training cannot advance the metadata checkpoint LSN");
			}
			return LogSequenceNumber(current + 1U);
		}

		[[nodiscard]] uint64_t derivedVectorIdentity(uint64_t prefix, PageId allocatorId) {
			if (!allocatorId.isValid() || (allocatorId.value() & RSS_DERIVED_MAPPING_INDEX_TYPE_MASK) != 0U) {
				failVectorTraining(FormatError::FERR_ARITHMETIC_OVERFLOW,
								   "relocatable Vector training exhausted its derived logical identity namespace");
			}
			return prefix | allocatorId.value();
		}

		[[nodiscard]] uint64_t decodeCanonicalNodeMappingKey(std::span<const std::byte> key) {
			if (key.size() != sizeof(uint64_t)) {
				failVectorTraining(FormatError::FERR_INVALID_HEADER,
								   "relocatable Vector training found a noncanonical Node-to-Vector mapping key");
			}
			uint64_t nodeId = 0U;
			for (const std::byte byte: key) {
				nodeId = (nodeId << 8U) | std::to_integer<uint8_t>(byte);
			}
			const std::vector<std::byte> canonical = VectorSemanticCodec::nodeMappingKey(nodeId);
			if (nodeId == 0U || !std::ranges::equal(key, canonical)) {
				failVectorTraining(FormatError::FERR_INVALID_HEADER,
								   "relocatable Vector training found a noncanonical Node-to-Vector mapping key");
			}
			return nodeId;
		}

		[[nodiscard]] uint64_t decodeCanonicalVectorMappingKey(std::span<const std::byte> key) {
			if (key.size() != sizeof(uint64_t)) {
				failVectorTraining(FormatError::FERR_INVALID_HEADER,
								   "relocatable Vector training found a noncanonical Vector-to-Node mapping key");
			}
			uint64_t vectorId = 0U;
			for (const std::byte byte: key) {
				vectorId = (vectorId << 8U) | std::to_integer<uint8_t>(byte);
			}
			const std::vector<std::byte> canonical = VectorSemanticCodec::vectorMappingKey(vectorId);
			if (vectorId == 0U || !std::ranges::equal(key, canonical)) {
				failVectorTraining(FormatError::FERR_INVALID_HEADER,
								   "relocatable Vector training found a noncanonical Vector-to-Node mapping key");
			}
			return vectorId;
		}

		[[nodiscard]] bool hasAnyPqIdentity(const VectorManifest &manifest) {
			return manifest.codebook_id != 0U || manifest.codebook_generation.isValid() ||
				   manifest.pq_mapping_index_id != 0U || manifest.pq_mapping_root_page_id.isValid() ||
				   manifest.pq_mapping_root_generation.isValid();
		}

		[[nodiscard]] bool hasCompletePqIdentity(const VectorManifest &manifest) {
			return manifest.codebook_id != 0U && manifest.codebook_generation.isValid() &&
				   VectorPqMapping::isMappingIndexId(manifest.pq_mapping_index_id) &&
				   manifest.pq_mapping_root_page_id.isValid() && manifest.pq_mapping_root_generation.isValid();
		}

		[[nodiscard]] uint32_t selectPqSubspaceCount(uint32_t dimensions) {
			const uint32_t preferred = std::max<uint32_t>(1U, dimensions / 8U);
			for (uint32_t candidate = preferred; candidate > 1U; --candidate) {
				if (dimensions % candidate == 0U) {
					return candidate;
				}
			}
			return 1U;
		}

		class RelocatableLogicalGraphTransaction final : public LogicalGraphTransaction,
														 public LogicalGraphTransactionVectorSnapshotProvider,
														 public LogicalGraphBoundedBulkEdgeTransaction {
		public:
			RelocatableLogicalGraphTransaction(RelocatableStorageBackend &backend, bool readOnly,
											   std::unique_ptr<LogicalGraphVectorSearchSnapshot> snapshot,
											   LogicalGraphMutationFence fence, Generation metadataGeneration) :
				backend_(backend), read_only_(readOnly), snapshot_(std::move(snapshot)), fence_(std::move(fence)),
				metadata_generation_(metadataGeneration), issued_node_ids_(), issued_edge_ids_() {
				if (snapshot_ == nullptr) { // transaction construction only receives a snapshot opened by the initialized native backend.
					throw std::logic_error("relocatable logical transaction could not open its read snapshot");
				}
				if (!fence_.isValid()) { // the public transaction factory derives this fence from the validated snapshot generation.
					throw std::logic_error("relocatable logical transaction requires a valid graph-state read fence");
				}
				if (!metadata_generation_.isValid()) { // metadata generation comes from the same validated snapshot as the transaction fence.
					throw std::logic_error("relocatable logical transaction requires a valid metadata generation");
				}
			}

			~RelocatableLogicalGraphTransaction() override {
				if (state_ == LogicalGraphTransactionState::LGTTS_ACTIVE) {
					rollback();
				}
			}

			[[nodiscard]] LogicalGraphTransactionState state() const override { return state_; }
			[[nodiscard]] bool isReadOnly() const override { return read_only_; }

			[[nodiscard]] LogicalGraphReadSnapshot &readSnapshot() override {
				requireActive("read its snapshot");
				if (hasStreamedBulkEdges()) {
					throw std::logic_error(
							"relocatable streamed bulk Edge transaction does not support read-your-writes reads");
				}
				return *snapshot_;
			}

			[[nodiscard]] LogicalGraphVectorSearchSnapshot &pinnedLogicalGraphVectorSearchSnapshot() override {
				requireActive("read its Vector snapshot");
				if (hasStreamedBulkEdges()) {
					throw std::logic_error("relocatable streamed bulk Edge transaction does not support Vector "
										   "read-your-writes reads");
				}
				return *snapshot_;
			}

			[[nodiscard]] LogicalGraphMutationBatchResult
			previewMutationBatch(const LogicalGraphMutationBatch &batch) const override {
				requireActive("preview a mutation batch");
				if (read_only_) {
					throw std::logic_error("relocatable read-only logical transaction cannot preview graph mutations");
				}
				if (batch.node_creates.empty() && batch.edge_creates.empty()) {
					return {};
				}
				if (!preview_reservation_.has_value()) {
					preview_reservation_.emplace(PendingCreationReservation{0U, 0U, {}});
					preview_reservation_->node_count = batch.node_creates.size();
					preview_reservation_->edge_count = batch.edge_creates.size();
					preview_reservation_->reservation = backend_.reserveLogicalGraphCreationIds(
							static_cast<uint64_t>(preview_reservation_->node_count),
							static_cast<uint64_t>(preview_reservation_->edge_count));
				} else if (preview_reservation_->node_count != batch.node_creates.size() ||
						   preview_reservation_->edge_count != batch.edge_creates.size()) {
					throw std::logic_error("relocatable logical transaction must stage or discard its outstanding "
										   "creation preview first");
				}
				return provisionalResult(preview_reservation_->reservation, batch.node_creates.size(),
										 batch.edge_creates.size());
			}

			[[nodiscard]] LogicalGraphMutationBatchResult stageMutationBatch(LogicalGraphMutationBatch batch) override {
				return stageMutationBatchWithPropertyPatches(std::move(batch), {}, 0U);
			}

			[[nodiscard]] bool supportsPropertyPatchStaging() const override { return true; }

			[[nodiscard]] LogicalGraphMutationBatchResult stageMutationBatchWithPropertyPatches(
				LogicalGraphMutationBatch batch, std::vector<LogicalGraphPropertyPatchRequest> patches,
				size_t maximumPropertyEntries, const LogicalGraphMutationReadControl &control = {}) override {
				requireActive("stage a mutation batch");
				if (control.checkpoint) control.checkpoint();
				if (hasStreamedBulkEdges()) {
					throw std::logic_error(
							"relocatable streamed bulk Edge transaction cannot stage ordinary overlay mutations");
				}
				if (read_only_) {
					throw std::logic_error("relocatable read-only logical transaction cannot stage graph mutations");
				}
				if (batch.creation_identity_plan.has_value()) {
					throw std::invalid_argument(
							"relocatable logical transaction owns creation identity reservation normalization");
				}
				size_t patchEntries = validateStagedPropertyPatches(batch, patches, maximumPropertyEntries, control);
				if (maximumPropertyEntries > (std::numeric_limits<size_t>::max)() - property_patch_entry_bound_)
					throw std::overflow_error("logical transaction Property patch entry bound overflowed");
				const size_t nodeOffset = issued_node_ids_.size();
				const size_t edgeOffset = issued_edge_ids_.size();
				NativeGraphLogicalIdReservation reservation;
				bool reservedForThisStage = false;
				if (preview_reservation_.has_value()) {
					if (preview_reservation_->node_count != batch.node_creates.size() ||
						preview_reservation_->edge_count != batch.edge_creates.size()) {
						throw std::logic_error("relocatable logical transaction stage does not match its outstanding "
											   "creation preview");
					}
					// Keep the preview capability intact until the complete statement
					// validates. A failed statement may be corrected and retried with
					// the same preview shape without leaking its outstanding lease.
					reservation = preview_reservation_->reservation;
				} else if (!batch.node_creates.empty() || !batch.edge_creates.empty()) {
					reservation =
							backend_.reserveLogicalGraphCreationIds(static_cast<uint64_t>(batch.node_creates.size()),
																	static_cast<uint64_t>(batch.edge_creates.size()));
					reservedForThisStage = true;
				}
				try {
					const LogicalGraphMutationBatchResult result =
							provisionalResult(reservation, batch.node_creates.size(), batch.edge_creates.size());
					std::vector<uint64_t> nextIssuedNodeIds = issued_node_ids_;
					std::vector<uint64_t> nextIssuedEdgeIds = issued_edge_ids_;
					std::vector<LogicalGraphCreationIdentityRange> nextNodeIdRanges = node_id_ranges_;
					std::vector<LogicalGraphCreationIdentityRange> nextEdgeIdRanges = edge_id_ranges_;
					LogicalGraphMutationBatch nextAccumulated = accumulated_;
					std::vector<LogicalGraphPropertyPatchRequest> nextPatches = property_patches_;
					rebaseCreatedReferences(batch, issued_node_ids_, issued_edge_ids_);
					for (auto &patch : patches)
						rebaseReference(patch.entity, nodeOffset, edgeOffset, issued_node_ids_, issued_edge_ids_);
					appendIssuedIds(reservation.node_ranges, nextIssuedNodeIds, nextNodeIdRanges, "Node");
					appendIssuedIds(reservation.edge_ranges, nextIssuedEdgeIds, nextEdgeIdRanges, "Edge");
					foldCreatedEntityFinalState(batch, nodeOffset, edgeOffset, nextAccumulated);
					// Complete-map replacement resets all older field operations.
					for (const auto &replacement : batch.property_replacements)
						std::erase_if(nextPatches, [&](const auto &patch) { return patch.entity == replacement.entity; });
					append(nextAccumulated, batch);
					foldStagedPropertyPatches(nextAccumulated, nextPatches, patches,
						patchEntries, maximumPropertyEntries, control);
					debug::PerfTrace::addValue("native_graph.write.transaction_retained_property_targets",
						static_cast<int64_t>(std::min(nextAccumulated.property_replacements.size(),
							static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
					debug::PerfTrace::addValue("native_graph.write.transaction_retained_label_targets",
						static_cast<int64_t>(std::min(nextAccumulated.node_label_replacements.size(),
							static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
					size_t retainedEntries = 0U;
					for (const auto &patch : nextPatches)
						for (size_t count : {patch.sets.size(), patch.removals.size()}) {
							if (count > (std::numeric_limits<size_t>::max)() - retainedEntries)
								throw std::overflow_error("logical transaction retained Property entries overflowed");
							retainedEntries += count;
						}
					debug::PerfTrace::addValue("native_graph.write.transaction_retained_patch_targets",
						static_cast<int64_t>(std::min(nextPatches.size(),
							static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
					debug::PerfTrace::addValue("native_graph.write.transaction_retained_patch_entries",
						static_cast<int64_t>(std::min(retainedEntries,
							static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));

					if (control.checkpoint) control.checkpoint();
					issued_node_ids_ = std::move(nextIssuedNodeIds);
					issued_edge_ids_ = std::move(nextIssuedEdgeIds);
					node_id_ranges_ = std::move(nextNodeIdRanges);
					edge_id_ranges_ = std::move(nextEdgeIdRanges);
					accumulated_ = std::move(nextAccumulated);
					property_patches_ = std::move(nextPatches);
					property_patch_entry_bound_ += maximumPropertyEntries;
					if (preview_reservation_.has_value()) {
						preview_reservation_.reset();
					}
					return result;
				} catch (...) {
					if (reservedForThisStage) {
						backend_.abandonLogicalGraphCreationIds(reservation);
					}
					throw;
				}
			}

			[[nodiscard]] std::vector<uint64_t>
			appendPersistedBulkEdges(const LogicalGraphPersistedBulkEdgeAppendRequest &request) override {
				requireActive("append streamed bulk Edges");
				if (read_only_) {
					throw std::logic_error("relocatable read-only logical transaction cannot append bulk Edges");
				}
				// The first append establishes the streamed-only transaction mode. Later
				// Driver ABI writes append to the same bounded scratch writer and must
				// retain their contiguous ID stream, while ordinary overlay work is still
				// rejected by stageMutationBatch().
				if (!hasStreamedBulkEdges() && (!isEmpty(accumulated_) || !property_patches_.empty() || !issued_node_ids_.empty() ||
												!issued_edge_ids_.empty() || preview_reservation_.has_value())) {
					throw std::logic_error("relocatable streamed bulk Edge transaction requires an otherwise empty "
										   "logical transaction");
				}
				if (request.sourceCount() != request.targetCount() || request.sourceCount() == 0U ||
					request.sourceCount() > backend_.maximumNativeBulkEdgeAppendRecords() ||
					request.sourceCount() >
							backend_.maximumNativeBulkEdgeTransactionRecords() - streamed_edge_record_count_) {
					throw std::invalid_argument("relocatable streamed bulk Edge append has invalid endpoint dimensions "
												"or exceeds its bound");
				}
				for (const LogicalGraphBulkEdgePropertyColumn &column: request.property_columns) {
					if (column.key.empty() || column.values.size() != request.sourceCount()) {
						throw std::invalid_argument(
								"relocatable streamed bulk Edge append has an invalid Property column");
					}
				}
				for (size_t column = 0U; column < request.property_columns.size(); ++column) {
					for (size_t prior = 0U; prior < column; ++prior) {
						if (request.property_columns[column].key == request.property_columns[prior].key) {
							throw std::invalid_argument(
									"relocatable streamed bulk Edge append has duplicate Property keys");
						}
					}
				}

				// Reject every row-local error before acquiring a durable lease or
				// writing scratch, so failed input has no partial side effect. Resolve
				// each distinct endpoint once per bounded append; sorting lets the native
				// Directory perform one ordered descent instead of one lookup per row.
				std::vector<uint64_t> endpointIds;
				endpointIds.reserve(request.sourceCount() * 2U);
				for (size_t row = 0U; row < request.sourceCount(); ++row) {
					const uint64_t sourceNodeId = request.sourceNodeIdAt(row);
					const uint64_t targetNodeId = request.targetNodeIdAt(row);
					if (sourceNodeId == 0U || targetNodeId == 0U) {
						throw std::invalid_argument("relocatable streamed bulk Edge append has a zero endpoint");
					}
					endpointIds.push_back(sourceNodeId);
					endpointIds.push_back(targetNodeId);
				}
				std::ranges::sort(endpointIds);
				endpointIds.erase(std::unique(endpointIds.begin(), endpointIds.end()), endpointIds.end());
				std::vector<bool> endpointPresent;
				if (snapshot_->supportsNodeExistenceProbes()) {
					endpointPresent = snapshot_->containsNodesSorted(endpointIds);
					if (endpointPresent.size() != endpointIds.size()) {
						throw std::logic_error(
								"relocatable streamed bulk Edge endpoint existence cardinality changed");
					}
				} else {
					endpointPresent.reserve(endpointIds.size());
					for (const uint64_t endpointId : endpointIds) {
						endpointPresent.push_back(snapshot_->readNodeShaped(endpointId, {}).node_id == endpointId);
					}
				}
				for (size_t index = 0U; index < endpointIds.size(); ++index) {
					if (!endpointPresent[index]) {
						throw std::invalid_argument(
								"relocatable streamed bulk Edge append has a missing persisted endpoint");
					}
				}
				size_t appendEncodedBytes = 0U;
				for (size_t row = 0U; row < request.sourceCount(); ++row) {
					const uint64_t sourceNodeId = request.sourceNodeIdAt(row);
					const uint64_t targetNodeId = request.targetNodeIdAt(row);
					NativeBulkEdgeRecord probe;
					probe.edge_id = 1U;
					probe.source_node_id = sourceNodeId;
					probe.target_node_id = targetNodeId;
					probe.type = request.type;
					probe.properties.reserve(request.property_columns.size());
					for (const LogicalGraphBulkEdgePropertyColumn &column: request.property_columns) {
						probe.properties.push_back({std::string(column.key), column.values[row]});
					}
					const size_t encodedBytes = NativeBulkEdgeRecordCodec::encodedSize(probe);
					if (encodedBytes > backend_.maximumNativeBulkEdgeAppendBytes() - appendEncodedBytes) {
						throw std::invalid_argument(
								"relocatable streamed bulk Edge append exceeds its encoded-byte bound");
					}
					appendEncodedBytes += encodedBytes;
				}

				try {
					if (bulk_session_lease_ == nullptr) {
						bulk_session_lease_ = backend_.tryAcquireNativeBulkSessionLease();
						if (bulk_session_lease_ == nullptr) {
							throw LogicalGraphMutationConflict(
									"relocatable streamed bulk Edge session conflicts with another active bulk session");
						}
					}
					ensureStreamedEdgeLease(request.sourceCount());
					std::vector<uint64_t> ids = issueStreamedBulkEdgeIds(request.sourceCount());
					{
						auto publicationFence = backend_.acquireNativePublicationGate();
						ensureBulkEdgeScratchWriter();
						bulk_edge_writer_->refreshCheckpointFloor(backend_.nextNativeBulkScratchCheckpointLsn());
						for (size_t row = 0U; row < ids.size(); ++row) {
							NativeBulkEdgeRecord record;
							record.edge_id = ids[row];
							record.source_node_id = request.sourceNodeIdAt(row);
							record.target_node_id = request.targetNodeIdAt(row);
							record.type = request.type;
							record.properties.reserve(request.property_columns.size());
							for (const LogicalGraphBulkEdgePropertyColumn &column: request.property_columns) {
								record.properties.push_back({std::string(column.key), column.values[row]});
							}
							bulk_edge_writer_->add(std::move(record));
						}
						// Leasing IDs and sealing scratch advance metadata through this
						// transaction's own handle. Treat that generation as the local
						// baseline; commit only rebases a later external publication.
						metadata_generation_ = backend_.currentNativeMetadataGeneration();
					}
					streamed_edge_record_count_ += ids.size();
					return ids;
				} catch (...) {
					failStreamedBulkEdgeTransaction();
					throw;
				}
			}

			[[nodiscard]] bool hasStreamedBulkEdges() const override { return bulk_edge_writer_ != nullptr; }

			[[nodiscard]] LogicalGraphMutationBatchResult commit() override {
				requireActive("commit");
				if (read_only_) {
					if (!isEmpty(accumulated_) || !property_patches_.empty()) { // read-only entry points reject writes before staging.
						throw std::logic_error(
								"relocatable read-only logical transaction contains staged graph mutations");
					}
					state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
					snapshot_.reset();
					return {};
				}
				if (hasStreamedBulkEdges()) {
					std::exception_ptr cleanup_failure;
					try {
						snapshot_.reset();
						const bool maintenanceAttached = backend_.maintenanceEnabled();
						const bool maintenanceQuiesced =
								backend_.currentNativeMetadataGeneration() != metadata_generation_ &&
								backend_.quiesceNativeMaintenanceForExternalRebase();
						{
							auto publicationFence = backend_.acquireNativePublicationGate();
							// A streamed transaction never exposes reads after its first
							// append. Release its original snapshot before rebasing the
							// process-local metadata-pool registry to another handle's root.
						if (backend_.currentNativeMetadataGeneration() != metadata_generation_) {
								backend_.refreshNativeMetadataAfterExternalPublication(maintenanceAttached &&
																					   !maintenanceQuiesced);
							} else if (maintenanceQuiesced) {
								backend_.resumeMaintenance();
							}
							snapshot_ = backend_.openLogicalGraphVectorSearchSnapshot();
						if (snapshot_ == nullptr) { // streamed rebase reopens the snapshot through the initialized backend before continuing.
								throw std::logic_error("relocatable streamed bulk Edge transaction could not rebase "
													   "its read snapshot");
							}
							bulk_edge_writer_->refreshCheckpointFloor(backend_.nextNativeBulkScratchCheckpointLsn());
							bulk_edge_manifest_ = bulk_edge_writer_->finish();
						}
						const NativeBulkEdgeRecordSource source = nativeBulkEdgeRecordSource(
								*bulk_edge_store_, bulk_edge_manifest_->location, BULK_EDGE_MANIFEST_DEPTH);
						NativeStreamedGraphPublicationPlan plan;
						if (backend_.currentNativeLogicalGraphMutationFence() == fence_) {
							plan = NativeStreamedGraphPublicationPlan::derive(
									source, streamed_edge_reservation_.edge_ranges,
									backend_.nativeBulkEdgePublicationPreflightOptions(), [this](uint64_t nodeId) {
										return snapshot_->supportsNodeExistenceProbes()
												   ? snapshot_->containsNode(nodeId)
											   : snapshot_->readNodeShaped(nodeId, {}).node_id == nodeId;
									});
						}
						backend_.commitNativeStreamedBulkEdges(*bulk_edge_store_, bulk_edge_manifest_->location,
															   streamed_edge_reservation_, plan, fence_);
						bulk_edge_writer_.reset();
						bulk_edge_store_.reset();
						bulk_edge_manifest_.reset();
						streamed_edge_reservation_ = {};
						bulk_session_lease_.reset();
						state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
						snapshot_.reset();
						return {};
					} catch (...) {
						cleanup_failure = std::current_exception();
					}
					try {
						failStreamedBulkEdgeTransaction();
					} catch (...) {
						// Preserve the publication/preflight failure as the caller-visible
						// cause. The cleanup path has already fenced this handle for reopen.
					}
					state_ = backend_.hasIndeterminateWriteOutcome() ? LogicalGraphTransactionState::LGTTS_INDETERMINATE
																	 : LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
					snapshot_.reset();
					if (cleanup_failure != nullptr) {
						std::rethrow_exception(cleanup_failure);
					}
					throw std::logic_error( // the preceding catch always records the publication exception before cleanup.
							"relocatable streamed bulk Edge commit requires the native streaming materializer");
				}

				try {
					CanonicalMutation canonical = canonicalizeFinalState();
					if ((!canonical.batch.property_replacements.empty() || !property_patches_.empty()) &&
						backend_.currentNativeLogicalGraphMutationFence() != fence_)
						throw LogicalGraphMutationConflict("logical transaction Property source fence changed before final planning");
					CanonicalPropertyPatches patches = extractFinalPropertyPatches(canonical.batch);
					appendCanonicalStagedPropertyPatches(canonical.batch, patches);
					LogicalGraphMutationBatchResult result = patches.requests.empty()
						? backend_.executeLogicalGraphMutationBatch(canonical.batch, fence_)
						: backend_.executeLogicalGraphMutationBatchWithPropertyPatches(
							*snapshot_, canonical.batch, patches.requests, patches.entry_bound, fence_);
					if (result != canonical.durable_result) { // the executor returns the canonical result produced from the same fenced batch.
						throw std::logic_error(
								"relocatable logical transaction publication changed its canonical creation identity");
					}
					abandonOutstandingCreationReservations();
					state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
					property_patches_.clear();
					property_patch_entry_bound_ = 0U;
					snapshot_.reset();
					return result;
				} catch (const LogicalGraphMutationConflict &) {
					abandonOutstandingCreationReservations();
					state_ = LogicalGraphTransactionState::LGTTS_CONFLICTED;
					accumulated_ = {};
					property_patches_.clear();
					property_patch_entry_bound_ = 0U;
					snapshot_.reset();
					throw;
				} catch (...) {
					// A failed logical commit must not leave a caller with a reusable
					// transaction whose staged state may no longer be meaningful.
					abandonOutstandingCreationReservations();
					state_ = backend_.hasIndeterminateWriteOutcome()
						? LogicalGraphTransactionState::LGTTS_INDETERMINATE
						: LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
					accumulated_ = {};
					property_patches_.clear();
					property_patch_entry_bound_ = 0U;
					snapshot_.reset();
					throw;
				}
			}

			void rollback() override {
				if (state_ != LogicalGraphTransactionState::LGTTS_ACTIVE) {
					throw std::logic_error("relocatable logical transaction is no longer active");
				}
				std::exception_ptr cleanup_failure;
				try {
					failStreamedBulkEdgeTransaction();
				} catch (...) {
					cleanup_failure = std::current_exception();
				}
				abandonOutstandingCreationReservations();
				accumulated_ = {};
				property_patches_.clear();
				property_patch_entry_bound_ = 0U;
				snapshot_.reset();
				state_ = LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
				if (cleanup_failure != nullptr) {
					std::rethrow_exception(cleanup_failure);
				}
			}

		private:
			static constexpr uint16_t BULK_EDGE_MANIFEST_DEPTH = 8U;

			void ensureStreamedEdgeLease(size_t requestedIds) {
				if (requestedIds == 0U) { // the public bulk append validates a positive row count before requesting a lease.
					throw std::logic_error("relocatable streamed bulk Edge lease requires positive work");
				}
				if (streamed_edge_next_id_ != 0U && streamed_edge_lease_last_id_ >= streamed_edge_next_id_ && // lease state is established and refreshed only by ensureStreamedEdgeLease.
					requestedIds <= streamed_edge_lease_last_id_ - streamed_edge_next_id_ + 1U) {
					return;
				}
				const size_t remainingRecordAllowance =
						backend_.maximumNativeBulkEdgeTransactionRecords() - streamed_edge_record_count_;
				const uint64_t leaseCount = static_cast<uint64_t>(
						std::min(remainingRecordAllowance,
								 std::max(requestedIds, backend_.nativeBulkEdgeIdLeaseBlockRecords())));
				NativeGraphLogicalIdReservation lease = backend_.reserveNativeBulkEdgeSessionIds(leaseCount);
					if (!lease.node_ranges.empty() || lease.edge_ranges.size() != 1U) { // reserveNativeBulkEdgeSessionIds constructs exactly one Edge-only range.
					backend_.abandonLogicalGraphCreationIds(lease);
					throw std::logic_error(
							"relocatable streamed bulk Edge transaction requires one exact durable lease");
				}
				const NativeGraphLogicalIdLeaseRange range = lease.edge_ranges.front();
					if (range.first_id == 0U || range.count != leaseCount || // the metadata lease API validates positive bounded ranges before returning.
					range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id) {
					backend_.abandonLogicalGraphCreationIds(lease);
					throw std::logic_error(
							"relocatable streamed bulk Edge transaction received an invalid durable ID lease");
				}
				if (!streamed_edge_reservation_.edge_ranges.empty()) {
					const NativeGraphLogicalIdLeaseRange previous = streamed_edge_reservation_.edge_ranges.back();
						if (previous.count - 1U > std::numeric_limits<uint64_t>::max() - previous.first_id || // stored lease ranges are validated and strictly ordered when acquired.
						previous.first_id + previous.count - 1U >= range.first_id) {
						backend_.abandonLogicalGraphCreationIds(lease);
						throw std::logic_error("relocatable streamed bulk Edge leases are not strictly ordered");
					}
				}
				try {
					streamed_edge_reservation_.edge_ranges.push_back(range);
				} catch (...) {
					backend_.abandonLogicalGraphCreationIds(lease);
					throw;
				}
				streamed_edge_next_id_ = range.first_id;
				streamed_edge_lease_last_id_ = range.first_id + range.count - 1U;
			}

			[[nodiscard]] std::vector<uint64_t> issueStreamedBulkEdgeIds(size_t count) {
					if (count == 0U || streamed_edge_next_id_ == 0U || // callers reserve and consume only positive IDs within the authenticated lease.
					streamed_edge_lease_last_id_ < streamed_edge_next_id_ ||
					count > streamed_edge_lease_last_id_ - streamed_edge_next_id_ + 1U) {
					throw std::logic_error("relocatable streamed bulk Edge transaction exhausted its durable ID lease");
				}
				std::vector<uint64_t> ids;
				ids.reserve(count);
				for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
					ids.push_back(streamed_edge_next_id_);
					streamed_edge_next_id_ = streamed_edge_next_id_ == std::numeric_limits<uint64_t>::max()
													 ? 0U
													 : streamed_edge_next_id_ + 1U;
				}
				return ids;
			}

			void ensureBulkEdgeScratchWriter() {
				if (bulk_edge_writer_ != nullptr) {
					return;
				}
				const uint64_t owner = backend_.allocateNativeBulkScratchOwner();
				auto store = backend_.createNativeBulkRunStore();
				auto writer = std::make_unique<NativeBulkEdgeRunStreamingWriter>(
						*store, owner,
						NativeBulkEdgeRunStreamingOptions{
								RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
								static_cast<uint64_t>(backend_.maximumNativeBulkEdgeTransactionRecords()),
								BULK_EDGE_MANIFEST_DEPTH},
						backend_.nextNativeBulkScratchCheckpointLsn());
				bulk_edge_store_ = std::move(store);
				bulk_edge_writer_ = std::move(writer);
			}

			void failStreamedBulkEdgeTransaction() {
				std::exception_ptr cleanup_failure;
				if (bulk_edge_writer_ != nullptr && bulk_edge_store_ != nullptr) { // these paired objects are created and retired together by the transaction.
					try {
						auto publicationFence = backend_.acquireNativePublicationGate();
						try {
							if (bulk_edge_manifest_.has_value()) {
								releaseNativeBulkEdgeManifest(*bulk_edge_store_, bulk_edge_manifest_->location,
															  backend_.nextNativeBulkScratchCheckpointLsn());
							} else {
								bulk_edge_writer_->refreshCheckpointFloor(backend_.nextNativeBulkScratchCheckpointLsn());
								bulk_edge_writer_->abort(backend_.nextNativeBulkScratchCheckpointLsn());
							}
						} catch (...) {
							// A sealed source may be malformed, or finish() may have been
							// interrupted after publishing scratch but before returning its
							// root. The held exclusive session lease proves all unpublished
							// scratch is safe to reclaim synchronously.
							bulk_edge_store_->reclaimUnpublishedOnOpen(backend_.nextNativeBulkScratchCheckpointLsn());
						}
					} catch (...) {
						cleanup_failure = std::current_exception();
						// Scratch is graph-unreachable, but an ambiguous allocator
						// publication still requires authoritative reopen recovery.
						backend_.requireReopenAfterNativeBulkStagingFailure();
					}
					bulk_edge_writer_.reset();
					bulk_edge_store_.reset();
					bulk_edge_manifest_.reset();
				}
				if (!streamed_edge_reservation_.empty()) {
					backend_.abandonLogicalGraphCreationIds(streamed_edge_reservation_);
					streamed_edge_reservation_ = {};
				}
				streamed_edge_next_id_ = 0U;
				streamed_edge_lease_last_id_ = 0U;
				streamed_edge_record_count_ = 0U;
				// Release only after scratch and its durable ID capability have been
				// retired, so a retry cannot race cleanup through another handle.
				bulk_session_lease_.reset();
				if (cleanup_failure != nullptr) {
					std::rethrow_exception(cleanup_failure);
				}
			}

			[[nodiscard]] static std::optional<size_t> findIssuedOrdinal(const std::vector<uint64_t> &ids,
																		 uint64_t id) {
				const auto iterator = std::lower_bound(ids.begin(), ids.end(), id);
				if (iterator == ids.end() || *iterator != id) {
					return std::nullopt;
				}
				return static_cast<size_t>(iterator - ids.begin());
			}

			static void rebaseReference(LogicalGraphMutationEntityReference &reference, size_t nodeOffset,
										size_t edgeOffset, const std::vector<uint64_t> &issuedNodeIds,
										const std::vector<uint64_t> &issuedEdgeIds) {
				if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE && // public mutation builders validate entity references before rebasing.
					reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE) {
					throw std::invalid_argument("relocatable logical transaction has an unknown entity kind");
				}
				uint64_t offset = 0U;
				switch (reference.reference_kind) {
					case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
						if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
							findIssuedOrdinal(issuedNodeIds, reference.value).has_value()) {
							reference.reference_kind = LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE;
							reference.value = *findIssuedOrdinal(issuedNodeIds, reference.value);
						} else if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
								   findIssuedOrdinal(issuedEdgeIds, reference.value).has_value()) {
							reference.reference_kind = LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE;
							reference.value = *findIssuedOrdinal(issuedEdgeIds, reference.value);
						}
						return;
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
						offset = static_cast<uint64_t>(nodeOffset);
						break;
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
						offset = static_cast<uint64_t>(edgeOffset);
						break;
					default:
						throw std::invalid_argument(
								"relocatable logical transaction has an unknown entity reference kind");
				}
				if (reference.value > std::numeric_limits<uint64_t>::max() - offset) { // public creation IDs are bounded by the metadata lease and cannot overflow rebasing.
					throw std::overflow_error("relocatable logical transaction batch reference ordinal overflow");
				}
				reference.value += offset;
			}

			static void rebaseCreatedReferences(LogicalGraphMutationBatch &batch,
												const std::vector<uint64_t> &issuedNodeIds,
												const std::vector<uint64_t> &issuedEdgeIds) {
				const size_t nodeOffset = issuedNodeIds.size();
				const size_t edgeOffset = issuedEdgeIds.size();
					if (nodeOffset > static_cast<size_t>(std::numeric_limits<uint64_t>::max()) || // transaction creation vectors are bounded by the uint64 logical-ID domain.
					edgeOffset > static_cast<size_t>(std::numeric_limits<uint64_t>::max())) {
					throw std::overflow_error(
							"relocatable logical transaction batch offset exceeds reference capacity");
				}
				for (LogicalGraphBatchEdgeCreateRequest &edge: batch.edge_creates) {
					rebaseReference(edge.source, nodeOffset, edgeOffset, issuedNodeIds, issuedEdgeIds);
					rebaseReference(edge.target, nodeOffset, edgeOffset, issuedNodeIds, issuedEdgeIds);
				}
				for (LogicalGraphPropertyMutationRequest &property: batch.property_replacements) {
					rebaseReference(property.entity, nodeOffset, edgeOffset, issuedNodeIds, issuedEdgeIds);
				}
				for (LogicalGraphNodeLabelMutationRequest &label: batch.node_label_replacements) {
					rebaseReference(label.node, nodeOffset, edgeOffset, issuedNodeIds, issuedEdgeIds);
				}
			}

			[[nodiscard]] static std::vector<uint64_t>
			collectIssuedIds(std::span<const NativeGraphLogicalIdLeaseRange> ranges, size_t expectedCount,
							 const char *entityName) {
				std::vector<uint64_t> ids;
				ids.reserve(expectedCount);
				for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
						if (range.first_id == 0U || range.count == 0U || // metadata-issued ranges are validated before canonicalization.
						range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id ||
						range.count > expectedCount - ids.size()) {
						throw std::overflow_error(std::string("relocatable logical transaction ") + entityName +
												  " received an invalid durable ID lease");
					}
					for (uint64_t offset = 0U; offset < range.count; ++offset) {
						ids.push_back(range.first_id + offset);
					}
				}
				if (ids.size() != expectedCount || !std::ranges::is_sorted(ids) || // canonicalization receives ranges from the validated reservation capability.
					std::ranges::adjacent_find(ids) != ids.end()) {
					throw std::logic_error(std::string("relocatable logical transaction ") + entityName +
										   " received a noncanonical durable ID lease");
				}
				return ids;
			}

			static void appendIssuedIds(std::span<const NativeGraphLogicalIdLeaseRange> ranges,
										std::vector<uint64_t> &ids,
										std::vector<LogicalGraphCreationIdentityRange> &identityRanges,
										const char *entityName) {
				for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
						if (range.first_id == 0U || range.count == 0U || // metadata-issued ranges are validated before canonicalization.
						range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id ||
						range.count > std::numeric_limits<size_t>::max() - ids.size() ||
						(!ids.empty() && ids.back() >= range.first_id)) {
						throw std::logic_error(std::string("relocatable logical transaction ") + entityName +
											   " received a noncanonical durable ID lease");
					}
					for (uint64_t offset = 0U; offset < range.count; ++offset) {
						ids.push_back(range.first_id + offset);
					}
					identityRanges.push_back({range.first_id, range.count});
				}
			}

			[[nodiscard]] static LogicalGraphMutationBatchResult
			provisionalResult(const NativeGraphLogicalIdReservation &reservation, size_t nodeCount, size_t edgeCount) {
				LogicalGraphMutationBatchResult result;
				result.created_node_ids = collectIssuedIds(reservation.node_ranges, nodeCount, "Node");
				result.created_edge_ids = collectIssuedIds(reservation.edge_ranges, edgeCount, "Edge");
				return result;
			}

			template<typename Mutation, typename Reference>
			static void mergeFinalReplacements(std::vector<Mutation> &destination, std::vector<Mutation> &source,
				const Reference &reference) {
				const auto target = [&](const Mutation &mutation) {
					const auto &entity = reference(mutation);
					return std::tuple{static_cast<uint8_t>(entity.entity_kind),
						static_cast<uint8_t>(entity.reference_kind), entity.value};
				};
				std::map<std::tuple<uint8_t, uint8_t, uint64_t>, size_t> positions;
				for (size_t index = 0U; index < destination.size(); ++index)
					positions.emplace(target(destination[index]), index);
				for (auto &replacement : source) {
					const auto key = target(replacement);
					if (const auto previous = positions.find(key); previous != positions.end()) {
						// The statement overlay already owns intermediate visibility.
						// Retain one authoritative final collection per entity here.
						destination[previous->second] = std::move(replacement);
					} else {
						positions.emplace(key, destination.size());
						destination.push_back(std::move(replacement));
					}
				}
			}

			static void append(LogicalGraphMutationBatch &destination, LogicalGraphMutationBatch &source) {
				destination.node_creates.insert(destination.node_creates.end(),
					std::make_move_iterator(source.node_creates.begin()), std::make_move_iterator(source.node_creates.end()));
				destination.edge_creates.insert(destination.edge_creates.end(),
					std::make_move_iterator(source.edge_creates.begin()), std::make_move_iterator(source.edge_creates.end()));
				mergeFinalReplacements(destination.property_replacements, source.property_replacements,
					[](const auto &mutation) -> const auto & { return mutation.entity; });
				mergeFinalReplacements(destination.node_label_replacements, source.node_label_replacements,
					[](const auto &mutation) -> const auto & { return mutation.node; });
				destination.edge_deletes.insert(destination.edge_deletes.end(),
												std::make_move_iterator(source.edge_deletes.begin()),
												std::make_move_iterator(source.edge_deletes.end()));
				destination.node_deletes.insert(destination.node_deletes.end(),
												std::make_move_iterator(source.node_deletes.begin()),
												std::make_move_iterator(source.node_deletes.end()));
			}

			static auto propertyTarget(const LogicalGraphMutationEntityReference &entity) {
				return std::tuple{static_cast<uint8_t>(entity.entity_kind),
					static_cast<uint8_t>(entity.reference_kind), entity.value};
			}

			static size_t validateStagedPropertyPatches(const LogicalGraphMutationBatch &batch,
				const std::vector<LogicalGraphPropertyPatchRequest> &patches, size_t maximumEntries,
				const LogicalGraphMutationReadControl &control) {
				const auto checkpoint = [&] { if (control.checkpoint) control.checkpoint(); };
				if (!patches.empty() && (maximumEntries == 0U || patches.size() > maximumEntries))
					throw std::invalid_argument("logical transaction Property patches require a positive work bound");
				size_t entries = 0U;
				std::set<std::tuple<uint8_t, uint8_t, uint64_t>> targets;
				for (const auto &patch : patches) {
					checkpoint();
					const auto &entity = patch.entity;
					const bool node = entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE;
					const bool edge = entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE;
					const bool valid = ((node || edge) && entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED && entity.value != 0U) ||
						(node && entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE && entity.value < batch.node_creates.size()) ||
						(edge && entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE && entity.value < batch.edge_creates.size());
					if (!valid || !targets.insert(propertyTarget(entity)).second)
						throw std::invalid_argument("logical transaction Property patches require distinct valid targets");
					for (size_t count : {patch.sets.size(), patch.removals.size()}) {
						if (count > maximumEntries - entries)
							throw std::length_error("logical transaction Property patches exceeded their entry bound");
						entries += count;
					}
					std::set<std::string_view> keys;
					for (const auto &field : patch.sets) {
						checkpoint();
						if (field.key.empty() || field.value.getType() == PropertyType::NULL_TYPE || !keys.insert(field.key).second)
							throw std::invalid_argument("logical transaction Property sets require unique non-null fields");
					}
					for (const auto &key : patch.removals) {
						checkpoint();
						if (key.empty() || !keys.insert(key).second)
							throw std::invalid_argument("logical transaction Property removals must be unique and disjoint");
					}
				}
				for (const auto &replacement : batch.property_replacements)
					if (targets.contains(propertyTarget(replacement.entity)))
						throw std::invalid_argument("logical transaction Property patches overlap complete replacements");
				for (const auto &deletion : batch.node_deletes)
					if (targets.contains(propertyTarget({LogicalGraphMutationEntityKind::LGMK_NODE,
						LogicalGraphMutationReferenceKind::LGRK_PERSISTED, deletion.node_id})))
						throw std::invalid_argument("logical transaction Property patch targets a deleted Node");
				for (const auto &deletion : batch.edge_deletes)
					if (targets.contains(propertyTarget({LogicalGraphMutationEntityKind::LGMK_EDGE,
						LogicalGraphMutationReferenceKind::LGRK_PERSISTED, deletion.edge_id})))
						throw std::invalid_argument("logical transaction Property patch targets a deleted Edge");
				return entries;
			}

			static void applyStagedPropertyPatch(std::vector<LogicalGraphWriteProperty> &properties,
				LogicalGraphPropertyPatchRequest &patch, const LogicalGraphMutationReadControl &control) {
				const auto checkpoint = [&] { if (control.checkpoint) control.checkpoint(); };
				std::map<std::string, PropertyValue> fields;
				for (auto &field : properties) {
					checkpoint();
					if (field.key.empty() || !fields.emplace(std::move(field.key), std::move(field.value)).second)
						throw std::invalid_argument("logical transaction Property source has duplicate or empty keys");
				}
				for (auto &field : patch.sets) { checkpoint(); fields.insert_or_assign(std::move(field.key), std::move(field.value)); }
				for (const auto &key : patch.removals) { checkpoint(); fields.erase(key); }
				properties.clear();
				properties.reserve(fields.size());
				for (auto &[key, value] : fields) { checkpoint(); properties.push_back({key, std::move(value)}); }
			}

			static void mergeStagedPropertyPatch(LogicalGraphPropertyPatchRequest &previous,
				LogicalGraphPropertyPatchRequest &next, const LogicalGraphMutationReadControl &control) {
				const auto checkpoint = [&] { if (control.checkpoint) control.checkpoint(); };
				std::map<std::string, std::optional<PropertyValue>> fields;
				for (auto &field : previous.sets) { checkpoint(); fields.emplace(std::move(field.key), std::move(field.value)); }
				for (auto &key : previous.removals) { checkpoint(); fields.emplace(std::move(key), std::nullopt); }
				for (auto &field : next.sets) { checkpoint(); fields.insert_or_assign(std::move(field.key), std::move(field.value)); }
				for (auto &key : next.removals) { checkpoint(); fields.insert_or_assign(std::move(key), std::nullopt); }
				previous.sets.clear();
				previous.removals.clear();
				for (auto &[key, value] : fields) {
					checkpoint();
					if (value) previous.sets.push_back({key, std::move(*value)});
					else previous.removals.push_back(key);
				}
			}

			static void foldStagedPropertyPatches(LogicalGraphMutationBatch &accumulated,
				std::vector<LogicalGraphPropertyPatchRequest> &retained,
				std::vector<LogicalGraphPropertyPatchRequest> &patches, size_t &entries, size_t maximumEntries,
				const LogicalGraphMutationReadControl &control) {
				for (auto &patch : patches) {
					if (control.checkpoint) control.checkpoint();
					std::vector<LogicalGraphWriteProperty> *source = nullptr;
					if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE)
						source = &accumulated.node_creates.at(static_cast<size_t>(patch.entity.value)).properties;
					else if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE)
						source = &accumulated.edge_creates.at(static_cast<size_t>(patch.entity.value)).properties;
					else {
						const auto full = std::ranges::find(accumulated.property_replacements,
							patch.entity, &LogicalGraphPropertyMutationRequest::entity);
						if (full != accumulated.property_replacements.end()) source = &full->properties;
					}
					if (source) {
						if (source->size() > maximumEntries - entries)
							throw std::length_error("logical transaction Property patch source exceeded its entry bound");
						entries += source->size();
						applyStagedPropertyPatch(*source, patch, control);
						continue;
					}
					const auto previous = std::ranges::find(retained, patch.entity, &LogicalGraphPropertyPatchRequest::entity);
					if (previous == retained.end()) retained.push_back(std::move(patch));
					else mergeStagedPropertyPatch(*previous, patch, control);
				}
			}

			void foldCreatedEntityFinalState(LogicalGraphMutationBatch &batch, size_t nodeOffset, size_t edgeOffset,
											 LogicalGraphMutationBatch &previouslyAccumulated) {
				const auto nodeCreate = [&previouslyAccumulated, &batch,
										 nodeOffset](uint64_t ordinal) -> LogicalGraphNodeCreateRequest & {
					if (ordinal < nodeOffset) {
						return previouslyAccumulated.node_creates.at(static_cast<size_t>(ordinal));
					}
					const uint64_t local = ordinal - nodeOffset;
					if (local >= batch.node_creates.size()) {
						throw std::invalid_argument(
								"relocatable logical transaction has an invalid created Node reference");
					}
					return batch.node_creates.at(static_cast<size_t>(local));
				};
				const auto edgeCreate = [&previouslyAccumulated, &batch,
										 edgeOffset](uint64_t ordinal) -> LogicalGraphBatchEdgeCreateRequest & {
					if (ordinal < edgeOffset) {
						return previouslyAccumulated.edge_creates.at(static_cast<size_t>(ordinal));
					}
					const uint64_t local = ordinal - edgeOffset;
					if (local >= batch.edge_creates.size()) {
						throw std::invalid_argument(
								"relocatable logical transaction has an invalid created Edge reference");
					}
					return batch.edge_creates.at(static_cast<size_t>(local));
				};

				std::vector<LogicalGraphPropertyMutationRequest> persistedProperties;
				persistedProperties.reserve(batch.property_replacements.size());
				std::set<std::tuple<uint8_t, uint64_t>> createdPropertyTargets;
				std::set<std::tuple<uint8_t, uint64_t>> persistedPropertyTargets;
				for (LogicalGraphPropertyMutationRequest &mutation: batch.property_replacements) {
					switch (mutation.entity.reference_kind) {
						case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
							if (mutation.entity.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
								!createdPropertyTargets
										 .insert({static_cast<uint8_t>(mutation.entity.reference_kind),
												  mutation.entity.value})
										 .second) {
								throw std::invalid_argument("relocatable logical transaction has duplicate or invalid "
															"created Node Properties");
							}
							nodeCreate(mutation.entity.value).properties = std::move(mutation.properties);
							break;
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
						if (mutation.entity.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
								!createdPropertyTargets
										 .insert({static_cast<uint8_t>(mutation.entity.reference_kind),
												  mutation.entity.value})
										 .second) {
								throw std::invalid_argument("relocatable logical transaction has duplicate or invalid "
															"created Edge Properties");
							}
							edgeCreate(mutation.entity.value).properties = std::move(mutation.properties);
							break;
						case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
							if (!persistedPropertyTargets
										 .insert({static_cast<uint8_t>(mutation.entity.entity_kind),
												  mutation.entity.value})
										 .second) {
								throw std::invalid_argument(
										"relocatable logical transaction has duplicate persisted Property replacements "
										"within one statement");
							}
							persistedProperties.push_back(std::move(mutation));
							break;
						default:
							throw std::invalid_argument(
									"relocatable logical transaction has an unknown Property reference kind");
					}
				}
				batch.property_replacements = std::move(persistedProperties);

				std::vector<LogicalGraphNodeLabelMutationRequest> persistedLabels;
				persistedLabels.reserve(batch.node_label_replacements.size());
				std::set<uint64_t> createdLabelTargets;
				std::set<uint64_t> persistedLabelTargets;
				for (LogicalGraphNodeLabelMutationRequest &mutation: batch.node_label_replacements) {
					switch (mutation.node.reference_kind) {
						case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
							if (mutation.node.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
								!createdLabelTargets.insert(mutation.node.value).second) {
								throw std::invalid_argument(
										"relocatable logical transaction has duplicate or invalid created Node labels");
							}
							nodeCreate(mutation.node.value).labels = std::move(mutation.labels);
							break;
						case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
							if (!persistedLabelTargets.insert(mutation.node.value).second) {
								throw std::invalid_argument("relocatable logical transaction has duplicate persisted "
															"Node label replacements "
															"within one statement");
							}
							persistedLabels.push_back(std::move(mutation));
							break;
						case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
						default:
							throw std::invalid_argument(
									"relocatable logical transaction has an unknown Node label reference kind");
					}
				}
				batch.node_label_replacements = std::move(persistedLabels);
			}

			struct CanonicalMutation {
				LogicalGraphMutationBatch batch;
				LogicalGraphMutationBatchResult durable_result;
			};

			struct CanonicalPropertyPatches {
				std::vector<LogicalGraphPropertyPatchRequest> requests;
				size_t entry_bound = 0U;
			};

			void appendCanonicalStagedPropertyPatches(const LogicalGraphMutationBatch &batch,
				CanonicalPropertyPatches &result) const {
				if (property_patches_.empty()) return;
				std::set<uint64_t> deletedNodes, detachedNodes, deletedEdges;
				for (const auto &deletion : batch.node_deletes) {
					deletedNodes.insert(deletion.node_id);
					if (deletion.detach) detachedNodes.insert(deletion.node_id);
				}
				for (const auto &deletion : batch.edge_deletes) deletedEdges.insert(deletion.edge_id);
				for (const auto &patch : property_patches_) {
					if (patch.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
						if (deletedNodes.contains(patch.entity.value)) continue;
					} else {
						if (deletedEdges.contains(patch.entity.value)) continue;
						if (!detachedNodes.empty()) {
							const auto edge = snapshot_->readEdgeShaped(patch.entity.value, {.include_type = false});
							if (detachedNodes.contains(edge.source_node_id) || detachedNodes.contains(edge.target_node_id)) continue;
						}
					}
					result.requests.push_back(patch);
				}
				if (property_patch_entry_bound_ > (std::numeric_limits<size_t>::max)() - result.entry_bound)
					throw std::overflow_error("logical transaction final Property patch bound overflowed");
				result.entry_bound += property_patch_entry_bound_;
			}

			// Stage/overlay continue to own complete visible collections. At commit,
			// derive the delta from the pinned base and authoritative final state;
			// never trust a caller-supplied hint about which fields changed.
			[[nodiscard]] CanonicalPropertyPatches extractFinalPropertyPatches(LogicalGraphMutationBatch &batch) const {
				CanonicalPropertyPatches result;
				std::vector<LogicalGraphPropertyMutationRequest> remaining;
				remaining.reserve(batch.property_replacements.size());
				result.requests.reserve(batch.property_replacements.size());
				for (auto &replacement : batch.property_replacements) {
					// Explicit NULL in a complete map is distinct from patch REMOVE.
					if (replacement.entity.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED ||
						std::ranges::any_of(replacement.properties, [](const auto &field) {
							return field.value.getType() == PropertyType::NULL_TYPE;
						})) {
						remaining.push_back(std::move(replacement));
						continue;
					}
					const EntityReadShape shape{.all_properties = true};
					std::vector<LogicalGraphPropertyEntry> source =
						replacement.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE
							? snapshot_->readNodeShaped(replacement.entity.value, shape).properties
							: snapshot_->readEdgeShaped(replacement.entity.value, shape).properties;
					std::ranges::sort(source, {}, &LogicalGraphPropertyEntry::key);
					std::vector<const LogicalGraphWriteProperty *> final;
					final.reserve(replacement.properties.size());
					for (const auto &field : replacement.properties) final.push_back(&field);
					std::ranges::sort(final, [](const auto *left, const auto *right) { return left->key < right->key; });
					for (size_t index = 0U; index < final.size(); ++index) {
						if (final[index]->key.empty() || (index != 0U && final[index - 1U]->key == final[index]->key))
							throw std::invalid_argument("logical transaction final properties require unique nonempty keys");
					}
					LogicalGraphPropertyPatchRequest patch{replacement.entity, {}, {}};
					size_t oldIndex = 0U, finalIndex = 0U;
					while (oldIndex < source.size() || finalIndex < final.size()) {
						if (finalIndex == final.size() || (oldIndex < source.size() && source[oldIndex].key < final[finalIndex]->key)) {
							patch.removals.push_back(source[oldIndex++].key);
						} else if (oldIndex == source.size() || final[finalIndex]->key < source[oldIndex].key) {
							patch.sets.push_back(*final[finalIndex++]);
						} else {
							if (source[oldIndex].value != final[finalIndex]->value) patch.sets.push_back(*final[finalIndex]);
							++oldIndex;
							++finalIndex;
						}
					}
					if (patch.sets.empty() && patch.removals.empty()) {
						// Retain the original request so a no-op commit still validates
						// its optimistic graph fence against an intervening writer.
						remaining.push_back(std::move(replacement));
						continue;
					}
					for (size_t count : {source.size(), patch.sets.size(), patch.removals.size()}) {
						if (count > (std::numeric_limits<size_t>::max)() - result.entry_bound)
							throw std::overflow_error("logical transaction Property patch entry bound overflowed");
						result.entry_bound += count;
					}
					result.requests.push_back(std::move(patch));
				}
				batch.property_replacements = std::move(remaining);
				return result;
			}

			struct PendingCreationReservation {
				size_t node_count = 0U;
				size_t edge_count = 0U;
				NativeGraphLogicalIdReservation reservation;
			};

			[[nodiscard]] CanonicalMutation canonicalizeFinalState() const {
				const size_t nodeCount = accumulated_.node_creates.size();
				const size_t edgeCount = accumulated_.edge_creates.size();
				if (issued_node_ids_.size() != nodeCount || issued_edge_ids_.size() != edgeCount) { // each creation request obtains its IDs before canonicalization.
					throw std::logic_error("relocatable logical transaction lost its durable creation identity");
				}
				std::vector<bool> cancelledNodes(nodeCount, false);
				std::vector<bool> cancelledEdges(edgeCount, false);
				std::vector<const LogicalGraphNodeDeleteRequest *> createdNodeDeletes(nodeCount, nullptr);
				std::set<uint64_t> uniqueNodeDeletes;
				std::set<uint64_t> persistedNodeDeletes;
				std::map<uint64_t, const LogicalGraphNodeDeleteRequest *> persistedNodeDeleteRequests;
				for (const LogicalGraphNodeDeleteRequest &request: accumulated_.node_deletes) {
					if (request.node_id == 0U || !uniqueNodeDeletes.insert(request.node_id).second ||
						(request.detach && (request.maximum_detach_edges == 0U ||
											request.maximum_detach_edges == std::numeric_limits<size_t>::max()))) {
						throw std::invalid_argument("relocatable logical transaction requires unique Node deletes and "
																"positive finite detach bounds");
					}
					const std::optional<size_t> ordinal = findIssuedOrdinal(issued_node_ids_, request.node_id);
					if (!ordinal.has_value()) {
						persistedNodeDeletes.insert(request.node_id);
						persistedNodeDeleteRequests.emplace(request.node_id, &request);
						continue;
					}
					cancelledNodes.at(*ordinal) = true;
					createdNodeDeletes.at(*ordinal) = &request;
				}
				std::set<uint64_t> uniqueEdgeDeletes;
				std::set<uint64_t> persistedEdgeDeletes;
				for (const LogicalGraphEdgeDeleteRequest &request: accumulated_.edge_deletes) {
					if (request.edge_id == 0U || !uniqueEdgeDeletes.insert(request.edge_id).second) {
						throw std::invalid_argument(
								"relocatable logical transaction requires unique nonzero Edge deletes");
					}
					if (const std::optional<size_t> ordinal = findIssuedOrdinal(issued_edge_ids_, request.edge_id);
						ordinal.has_value()) {
						cancelledEdges.at(*ordinal) = true;
					} else {
						persistedEdgeDeletes.insert(request.edge_id);
					}
				}

				const auto createdNodeOrdinal = [nodeCount](const LogicalGraphMutationEntityReference &reference,
															const char *operation) -> std::optional<size_t> {
					if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
						return std::nullopt;
					}
						if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
						reference.value >= nodeCount) {
						throw std::invalid_argument(std::string("relocatable logical transaction ") + operation +
													" has an invalid created Node reference");
					}
					return static_cast<size_t>(reference.value);
				};
				const auto persistedNodeDeletion =
						[&persistedNodeDeleteRequests](const LogicalGraphMutationEntityReference &reference)
						-> const LogicalGraphNodeDeleteRequest * {
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
						reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						return nullptr;
					}
					const auto iterator = persistedNodeDeleteRequests.find(reference.value);
					return iterator == persistedNodeDeleteRequests.end() ? nullptr : iterator->second;
				};
				for (size_t nodeOrdinal = 0U; nodeOrdinal < createdNodeDeletes.size(); ++nodeOrdinal) {
					const LogicalGraphNodeDeleteRequest *const deletion = createdNodeDeletes[nodeOrdinal];
					if (deletion == nullptr) {
						continue;
					}
					size_t liveIncidentCreatedEdges = 0U;
					for (size_t edgeOrdinal = 0U; edgeOrdinal < accumulated_.edge_creates.size(); ++edgeOrdinal) {
						const LogicalGraphBatchEdgeCreateRequest &edge = accumulated_.edge_creates[edgeOrdinal];
						const bool incident = (createdNodeOrdinal(edge.source, "Edge creation") == nodeOrdinal) ||
											  (createdNodeOrdinal(edge.target, "Edge creation") == nodeOrdinal);
						if (!incident || cancelledEdges[edgeOrdinal]) {
							continue;
						}
						if (!deletion->detach) {
							throw std::invalid_argument(
									"relocatable logical transaction cannot delete a newly created Node "
									"with a live Edge without detach semantics");
						}
						if (++liveIncidentCreatedEdges > deletion->maximum_detach_edges) {
							throw std::invalid_argument(
									"relocatable logical transaction detach delete exceeds its created Edge bound");
						}
					}
				}
				std::map<uint64_t, size_t> persistedDetachCreatedEdgeCounts;
				for (size_t edgeOrdinal = 0U; edgeOrdinal < accumulated_.edge_creates.size(); ++edgeOrdinal) {
					const LogicalGraphBatchEdgeCreateRequest &edge = accumulated_.edge_creates[edgeOrdinal];
					const LogicalGraphNodeDeleteRequest *const persistedSourceDeletion =
							persistedNodeDeletion(edge.source);
					const LogicalGraphNodeDeleteRequest *const persistedTargetDeletion =
							persistedNodeDeletion(edge.target);
					if (((persistedSourceDeletion != nullptr && !persistedSourceDeletion->detach) ||
						 (persistedTargetDeletion != nullptr && !persistedTargetDeletion->detach)) &&
						!cancelledEdges[edgeOrdinal]) {
						throw std::invalid_argument(
								"relocatable logical transaction cannot delete a persisted Node with a live newly "
								"created Edge without detach semantics");
					}
					const auto countPersistedDetach = [&persistedDetachCreatedEdgeCounts, edgeOrdinal,
													   &cancelledEdges](const LogicalGraphNodeDeleteRequest *deletion) {
						if (deletion == nullptr || !deletion->detach || cancelledEdges[edgeOrdinal]) {
							return;
						}
						size_t &count = persistedDetachCreatedEdgeCounts[deletion->node_id];
						if (++count > deletion->maximum_detach_edges) {
							throw std::invalid_argument(
									"relocatable logical transaction detach delete exceeds its created Edge bound");
						}
					};
					countPersistedDetach(persistedSourceDeletion);
					if (persistedTargetDeletion != persistedSourceDeletion) {
						countPersistedDetach(persistedTargetDeletion);
					}
					const std::optional<size_t> source = createdNodeOrdinal(edge.source, "Edge creation");
					const std::optional<size_t> target = createdNodeOrdinal(edge.target, "Edge creation");
					if ((source.has_value() && cancelledNodes[*source]) ||
						(target.has_value() && cancelledNodes[*target]) ||
						(persistedSourceDeletion != nullptr && persistedSourceDeletion->detach) ||
						(persistedTargetDeletion != nullptr && persistedTargetDeletion->detach)) {
						cancelledEdges[edgeOrdinal] = true;
					}
				}
				std::map<uint64_t, size_t> persistedDetachRemainingBudgets;
				for (const auto &[nodeId, deletion]: persistedNodeDeleteRequests) {
					if (!deletion->detach) {
						continue;
					}
					const size_t createdEdgeCount = persistedDetachCreatedEdgeCounts[nodeId];
					if (createdEdgeCount > deletion->maximum_detach_edges) {
						throw std::logic_error(
								"relocatable logical transaction lost its detach-created-Edge budget invariant");
					}
					const size_t remaining = deletion->maximum_detach_edges - createdEdgeCount;
					// Explicitly deleted persisted Edges are already absent from the
					// transaction's final graph. Read enough snapshot candidates to
					// skip them while still detecting one additional live incident Edge.
					const size_t deletedAllowance = persistedEdgeDeletes.size();
					if (deletedAllowance >= std::numeric_limits<size_t>::max() - remaining) {
						throw std::invalid_argument(
								"relocatable logical transaction detach delete cannot derive a bounded Edge scan");
					}
					const size_t scanBound = remaining + deletedAllowance + 1U;
					std::set<uint64_t> remainingPersistedEdges;
					const auto collect = [&remainingPersistedEdges, &persistedEdgeDeletes,
										  remaining](const LogicalGraphEdgeTopology &edge) {
						if (persistedEdgeDeletes.contains(edge.edge_id)) {
							return true;
						}
						remainingPersistedEdges.insert(edge.edge_id);
						if (remainingPersistedEdges.size() > remaining) {
							throw std::invalid_argument("relocatable logical transaction detach delete exceeds its "
														"total incident Edge bound");
						}
						return true;
					};
					const auto collectDirection = [&](LogicalGraphReadDirection direction) {
						const LogicalGraphAdjacencyTopologyPage page =
								snapshot_->scanAdjacentEdgeTopologies(nodeId, direction, {}, scanBound);
						for (const LogicalGraphEdgeTopology &edge: page.edges) {
							static_cast<void>(collect(edge));
						}
					};
					collectDirection(LogicalGraphReadDirection::LGRD_OUTGOING);
					collectDirection(LogicalGraphReadDirection::LGRD_INCOMING);
					persistedDetachRemainingBudgets.emplace(nodeId, remaining);
				}

				CanonicalMutation canonical;
				std::vector<size_t> nodeOrdinals(nodeCount, std::numeric_limits<size_t>::max());
				std::vector<size_t> edgeOrdinals(edgeCount, std::numeric_limits<size_t>::max());
				for (size_t ordinal = 0U; ordinal < nodeCount; ++ordinal) {
					if (cancelledNodes[ordinal]) {
						continue;
					}
					nodeOrdinals[ordinal] = canonical.batch.node_creates.size();
					canonical.batch.node_creates.push_back(accumulated_.node_creates[ordinal]);
					canonical.durable_result.created_node_ids.push_back(issued_node_ids_.at(ordinal));
				}
				for (size_t ordinal = 0U; ordinal < edgeCount; ++ordinal) {
					if (cancelledEdges[ordinal]) {
						continue;
					}
					edgeOrdinals[ordinal] = canonical.batch.edge_creates.size();
					canonical.batch.edge_creates.push_back(accumulated_.edge_creates[ordinal]);
					canonical.durable_result.created_edge_ids.push_back(issued_edge_ids_.at(ordinal));
				}
				const auto remapReference = [&nodeOrdinals,
											 &edgeOrdinals](LogicalGraphMutationEntityReference &reference,
															const char *operation) {
					switch (reference.reference_kind) {
						case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
							return;
						case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
							if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
								reference.value >= nodeOrdinals.size() ||
								nodeOrdinals[static_cast<size_t>(reference.value)] ==
										std::numeric_limits<size_t>::max()) {
								throw std::invalid_argument(std::string("relocatable logical transaction ") +
															operation +
															" references a cancelled or invalid created Node");
						}
						reference.value = nodeOrdinals[static_cast<size_t>(reference.value)];
						return;
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE: // foldCreatedEntityFinalState consumes valid created-Edge replacements and rejects malformed ones before canonical remapping.
						if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
								reference.value >= edgeOrdinals.size() ||
								edgeOrdinals[static_cast<size_t>(reference.value)] ==
										std::numeric_limits<size_t>::max()) {
								throw std::invalid_argument(std::string("relocatable logical transaction ") +
															operation +
															" references a cancelled or invalid created Edge");
							}
							reference.value = edgeOrdinals[static_cast<size_t>(reference.value)];
							return;
					}
					throw std::invalid_argument(std::string("relocatable logical transaction ") + operation +
												" has an unknown entity reference kind");
				};
				for (LogicalGraphBatchEdgeCreateRequest &request: canonical.batch.edge_creates) {
					remapReference(request.source, "Edge creation");
					remapReference(request.target, "Edge creation");
				}
				// A logical transaction accumulates statement final states, whereas the
				// native executor receives one final publication. Keep only the final
				// replacement for each persisted entity so successive SET/REMOVE-style
				// statements do not become duplicate targets in that one publication.
				std::map<std::tuple<uint8_t, uint64_t>, LogicalGraphPropertyMutationRequest>
						persistedPropertyReplacements;
				for (const LogicalGraphPropertyMutationRequest &source: accumulated_.property_replacements) {
					LogicalGraphPropertyMutationRequest request = source;
					if (request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						if ((request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
							 persistedNodeDeletes.contains(request.entity.value)) ||
							(request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
							 persistedEdgeDeletes.contains(request.entity.value))) {
							continue;
						}
						if (request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
							!persistedNodeDeleteRequests.empty()) {
							const auto edge = snapshot_->readEdgeShaped(request.entity.value, {.include_type = false});
							const auto detached = [&](uint64_t nodeId) {
								const auto found = persistedNodeDeleteRequests.find(nodeId);
								return found != persistedNodeDeleteRequests.end() && found->second->detach;
							};
							if (detached(edge.source_node_id) || detached(edge.target_node_id)) continue;
						}
						persistedPropertyReplacements.insert_or_assign(
								{static_cast<uint8_t>(request.entity.entity_kind), request.entity.value},
								std::move(request));
						continue;
					}
					if ((request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE && // every created-entity replacement is folded into its CREATE request before this final canonicalization pass.
						 request.entity.value < nodeOrdinals.size() &&
						 nodeOrdinals[static_cast<size_t>(request.entity.value)] ==
								 std::numeric_limits<size_t>::max()) ||
						(request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
						 request.entity.value < edgeOrdinals.size() &&
						 edgeOrdinals[static_cast<size_t>(request.entity.value)] ==
								 std::numeric_limits<size_t>::max())) {
						continue;
					}
					remapReference(request.entity, "Property replacement");
					canonical.batch.property_replacements.push_back(std::move(request));
				}
				for (auto &[key, request]: persistedPropertyReplacements) {
					static_cast<void>(key);
					canonical.batch.property_replacements.push_back(std::move(request));
				}
				std::map<uint64_t, LogicalGraphNodeLabelMutationRequest> persistedLabelReplacements;
				for (const LogicalGraphNodeLabelMutationRequest &source: accumulated_.node_label_replacements) {
					LogicalGraphNodeLabelMutationRequest request = source;
					if (request.node.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						if (persistedNodeDeletes.contains(request.node.value)) {
							continue;
						}
						persistedLabelReplacements.insert_or_assign(request.node.value, std::move(request));
						continue;
					}
					if (request.node.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE && // created-Node labels are folded into CREATE before final canonicalization.
						request.node.value < nodeOrdinals.size() &&
						nodeOrdinals[static_cast<size_t>(request.node.value)] == std::numeric_limits<size_t>::max()) {
						continue;
					}
					remapReference(request.node, "Node label replacement");
					canonical.batch.node_label_replacements.push_back(std::move(request));
				}
				for (auto &[nodeId, request]: persistedLabelReplacements) {
					static_cast<void>(nodeId);
					canonical.batch.node_label_replacements.push_back(std::move(request));
				}
				for (const LogicalGraphEdgeDeleteRequest &request: accumulated_.edge_deletes) {
					if (!findIssuedOrdinal(issued_edge_ids_, request.edge_id).has_value()) {
						canonical.batch.edge_deletes.push_back(request);
					}
				}
				for (const LogicalGraphNodeDeleteRequest &request: accumulated_.node_deletes) {
					if (!findIssuedOrdinal(issued_node_ids_, request.node_id).has_value()) {
						LogicalGraphNodeDeleteRequest canonicalRequest = request;
						if (request.detach) {
							const size_t remaining = persistedDetachRemainingBudgets.at(request.node_id);
							if (remaining == 0U) {
								// No durable incident Edge remains after final-state folding.
								// Use the isolated-delete form so the lower bounded scanner is
								// not asked to express a zero-record detach scan.
								canonicalRequest.detach = false;
							} else {
								canonicalRequest.maximum_detach_edges = remaining;
							}
						}
						canonical.batch.node_deletes.push_back(std::move(canonicalRequest));
					}
				}
				if (nodeCount != 0U || edgeCount != 0U) {
					LogicalGraphCreationIdentityPlan plan;
					if (nodeCount != 0U) {
						plan.node_id_ranges = node_id_ranges_;
						plan.node_ids = canonical.durable_result.created_node_ids;
					}
					if (edgeCount != 0U) {
						plan.edge_id_ranges = edge_id_ranges_;
						plan.edge_ids = canonical.durable_result.created_edge_ids;
					}
					canonical.batch.creation_identity_plan = std::move(plan);
				}
				return canonical;
			}

			static bool isEmpty(const LogicalGraphMutationBatch &batch) {
				return batch.node_creates.empty() && batch.edge_creates.empty() &&
					   batch.property_replacements.empty() && batch.node_label_replacements.empty() &&
					   batch.edge_deletes.empty() && batch.node_deletes.empty() &&
					   !batch.creation_identity_plan.has_value();
			}

			void abandonOutstandingCreationReservations() noexcept {
				NativeGraphLogicalIdReservation reservation;
				reservation.node_ranges.reserve(node_id_ranges_.size());
				reservation.edge_ranges.reserve(edge_id_ranges_.size());
				for (const LogicalGraphCreationIdentityRange &range: node_id_ranges_) {
					reservation.node_ranges.push_back({range.first_id, range.count});
				}
				for (const LogicalGraphCreationIdentityRange &range: edge_id_ranges_) {
					reservation.edge_ranges.push_back({range.first_id, range.count});
				}
				backend_.abandonLogicalGraphCreationIds(reservation);
				if (preview_reservation_.has_value()) {
					backend_.abandonLogicalGraphCreationIds(preview_reservation_->reservation);
					preview_reservation_.reset();
				}
			}

			void requireActive(const char *operation) const {
				if (state_ != LogicalGraphTransactionState::LGTTS_ACTIVE || snapshot_ == nullptr) {
					throw std::logic_error(std::string("relocatable logical transaction cannot ") + operation +
										   " after it has finished");
				}
			}

			RelocatableStorageBackend &backend_;
			bool read_only_ = false;
			LogicalGraphTransactionState state_ = LogicalGraphTransactionState::LGTTS_ACTIVE;
			std::unique_ptr<LogicalGraphVectorSearchSnapshot> snapshot_;
			LogicalGraphMutationFence fence_;
			Generation metadata_generation_{};
			LogicalGraphMutationBatch accumulated_;
			std::vector<LogicalGraphPropertyPatchRequest> property_patches_;
			size_t property_patch_entry_bound_ = 0U;
			mutable std::optional<PendingCreationReservation> preview_reservation_;
			std::vector<uint64_t> issued_node_ids_;
			std::vector<uint64_t> issued_edge_ids_;
			std::vector<LogicalGraphCreationIdentityRange> node_id_ranges_;
			std::vector<LogicalGraphCreationIdentityRange> edge_id_ranges_;
			std::unique_ptr<NativeBulkRunStore> bulk_edge_store_;
			std::unique_ptr<NativeBulkEdgeRunStreamingWriter> bulk_edge_writer_;
			std::unique_ptr<NativeBulkSessionLease> bulk_session_lease_;
			std::optional<NativeBulkEdgeRunManifest> bulk_edge_manifest_;
			NativeGraphLogicalIdReservation streamed_edge_reservation_;
			uint64_t streamed_edge_next_id_ = 0U;
			uint64_t streamed_edge_lease_last_id_ = 0U;
			size_t streamed_edge_record_count_ = 0U;
		};

		void appendLogicalGraphRootTokens(LogicalGraphMutationFence &fence, size_t &offset,
										  const PhysicalPageRef &root) {
			if (offset > fence.graph_state_tokens.size() - LogicalGraphMutationFence::GRAPH_STATE_TOKENS_PER_ROOT) {
				throw std::logic_error("relocatable logical graph fence root layout exceeds its fixed token capacity");
			}
			fence.graph_state_tokens[offset++] = root.physical_slot.value();
			fence.graph_state_tokens[offset++] = static_cast<uint64_t>(root.frame_index);
			fence.graph_state_tokens[offset++] = root.generation.value();
		}

		void requireExistingRegularFile(const std::filesystem::path &path, const char *purpose) {
			std::error_code error;
			if (path.empty() || !std::filesystem::is_regular_file(path, error)) {
				throw std::runtime_error(std::string("relocatable backend requires an existing regular ") + purpose +
										 " file");
			}
		}

		[[nodiscard]] std::filesystem::path canonicalExistingPath(const std::filesystem::path &path,
																  const char *purpose) {
			requireExistingRegularFile(path, purpose);
			return std::filesystem::canonical(path);
		}

		[[nodiscard]] std::filesystem::path canonicalNewPath(const std::filesystem::path &path, const char *purpose) {
			if (path.empty() || path.filename().empty()) {
				throw std::invalid_argument(std::string("relocatable backend requires a nonempty ") + purpose +
											" path");
			}
			const std::filesystem::path absolute = std::filesystem::absolute(path).lexically_normal();
			const std::filesystem::path parent = std::filesystem::canonical(absolute.parent_path());
			if (!std::filesystem::is_directory(parent)) {
				throw std::runtime_error(std::string("relocatable backend requires an existing ") + purpose +
										 " parent directory");
			}
			return parent / absolute.filename();
		}

		void validateBufferPoolCapacity(size_t capacityBytes) {
			if (capacityBytes < RSS_BUFFER_POOL_PAGE_BYTES) {
				throw std::invalid_argument(
						"relocatable backend buffer pool capacity must contain at least one 4 KiB page");
			}
		}

		[[nodiscard]] uint64_t generateDatabaseId() {
			static std::atomic<uint64_t> fallback{1U};
			try {
				std::random_device random;
				const uint64_t value = (static_cast<uint64_t>(random()) << 32U) | static_cast<uint64_t>(random());
				if (value != 0U) {
					return value;
				}
			} catch (const std::exception &) {
				// Database IDs are uniqueness tokens, not secrets. The monotonic
				// fallback preserves the nonzero on-disk invariant without entropy.
			}
			const uint64_t value = fallback.fetch_add(1U, std::memory_order_relaxed);
			return value == 0U ? fallback.fetch_add(1U, std::memory_order_relaxed) : value;
		}

		[[nodiscard]] bool sameIndexSchema(const IndexDefinition &definition, IndexDefinitionEntityKind entityKind,
										   IndexDefinitionKind kind, uint64_t labelTokenId,
										   std::span<const std::string> propertyNames) {
			return definition.entity_kind == entityKind && definition.kind == kind &&
				   definition.label_token_id == labelTokenId &&
				   definition.property_names.size() == propertyNames.size() &&
				   std::equal(definition.property_names.begin(), definition.property_names.end(),
							  propertyNames.begin());
		}

		[[nodiscard]] std::string defaultNodeIndexName(std::string_view label,
													   std::span<const std::string> propertyNames) {
			if (label.empty() || propertyNames.empty()) { // both callers validate a nonempty label and Property-key sequence before deriving a default name.
				throw std::invalid_argument("logical node Index requires a label and at least one Property key");
			}
			std::string result = "index_node_" + std::to_string(label.size()) + "_" + std::string(label);
			for (const std::string &property: propertyNames) {
				result += "_" + std::to_string(property.size()) + "_" + property;
			}
			if (result.size() > RSS_INDEX_DEFINITION_MAX_NAME_BYTES) {
				throw std::invalid_argument("logical node Index default name exceeds the durable name bound; supply a "
											"shorter explicit name");
			}
			return result;
		}

		[[nodiscard]] std::string normalizeVectorMetric(std::string_view metric) {
			if (metric.empty() || metric.size() > RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES) {
				throw std::invalid_argument("logical Vector Index requires a bounded nonempty distance metric");
			}
			std::string normalized;
			normalized.reserve(metric.size());
			for (const char character: metric) {
				const unsigned char byte = static_cast<unsigned char>(character);
				if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) {
					normalized.push_back(static_cast<char>(byte - static_cast<unsigned char>('A') +
														   static_cast<unsigned char>('a')));
				} else {
					normalized.push_back(character);
				}
			}
			if (normalized == "inner_product" || normalized == "innerproduct") {
				normalized = "ip";
			}
			if (normalized != "l2" && normalized != "ip" && normalized != "cosine") {
				throw std::invalid_argument("logical Vector Index requires metric l2, ip, or cosine");
			}
			return normalized;
		}

		[[nodiscard]] std::string defaultNodeVectorIndexName(std::string_view label, std::string_view property,
																 uint32_t dimensions, std::string_view metric) {
			std::string result = "index_vector_node_" + std::to_string(label.size()) + "_" + std::string(label) + "_" +
									 std::to_string(property.size()) + "_" + std::string(property) + "_" +
									 std::to_string(dimensions) + "_" + std::string(metric);
			if (result.size() > RSS_INDEX_DEFINITION_MAX_NAME_BYTES) {
				throw std::invalid_argument("logical Vector Index default name exceeds the durable name bound; supply "
											"a shorter explicit name");
			}
			return result;
		}

		[[nodiscard]] LogicalGraphIndexKind logicalIndexKind(IndexDefinitionKind kind) {
			switch (kind) {
				case IndexDefinitionKind::IDK_PROPERTY:
					return LogicalGraphIndexKind::LGIK_PROPERTY;
				case IndexDefinitionKind::IDK_COMPOSITE:
					return LogicalGraphIndexKind::LGIK_COMPOSITE;
				case IndexDefinitionKind::IDK_VECTOR:
					return LogicalGraphIndexKind::LGIK_VECTOR;
				case IndexDefinitionKind::IDK_LABEL:
					break;
			}
			throw FormatException(
					FormatError::FERR_INVALID_ENUM,
					"relocatable logical schema adapter does not expose a label/type Index as a Property Index");
		}

		[[nodiscard]] ConstraintDefinitionEntityKind
		constraintDefinitionEntityKind(LogicalGraphConstraintEntityKind kind) {
			switch (kind) {
				case LogicalGraphConstraintEntityKind::LGCEK_NODE:
					return ConstraintDefinitionEntityKind::CDEK_NODE;
				case LogicalGraphConstraintEntityKind::LGCEK_EDGE:
					return ConstraintDefinitionEntityKind::CDEK_EDGE;
			}
			throw std::invalid_argument("logical constraint has an unknown entity kind");
		}

		[[nodiscard]] ConstraintDefinitionKind constraintDefinitionKind(LogicalGraphConstraintKind kind) {
			switch (kind) {
				case LogicalGraphConstraintKind::LGCK_UNIQUE:
					return ConstraintDefinitionKind::CDK_UNIQUE;
				case LogicalGraphConstraintKind::LGCK_NOT_NULL:
					return ConstraintDefinitionKind::CDK_NOT_NULL;
				case LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE:
					return ConstraintDefinitionKind::CDK_PROPERTY_TYPE;
				case LogicalGraphConstraintKind::LGCK_NODE_KEY:
					return ConstraintDefinitionKind::CDK_NODE_KEY;
			}
			throw std::invalid_argument("logical constraint has an unknown kind");
		}

		[[nodiscard]] ConstraintDefinitionPropertyType
		constraintDefinitionPropertyType(LogicalGraphConstraintPropertyType type) {
			switch (type) {
				case LogicalGraphConstraintPropertyType::LGCPT_NONE:
					return ConstraintDefinitionPropertyType::CDPT_NONE;
				case LogicalGraphConstraintPropertyType::LGCPT_BOOLEAN:
					return ConstraintDefinitionPropertyType::CDPT_BOOLEAN;
				case LogicalGraphConstraintPropertyType::LGCPT_INTEGER:
					return ConstraintDefinitionPropertyType::CDPT_INTEGER;
				case LogicalGraphConstraintPropertyType::LGCPT_FLOAT:
					return ConstraintDefinitionPropertyType::CDPT_FLOAT;
				case LogicalGraphConstraintPropertyType::LGCPT_STRING:
					return ConstraintDefinitionPropertyType::CDPT_STRING;
				case LogicalGraphConstraintPropertyType::LGCPT_LIST:
					return ConstraintDefinitionPropertyType::CDPT_LIST;
				case LogicalGraphConstraintPropertyType::LGCPT_MAP:
					return ConstraintDefinitionPropertyType::CDPT_MAP;
			}
			throw std::invalid_argument("logical constraint has an unknown Property type");
		}

		[[nodiscard]] LogicalGraphConstraintDefinition
		logicalConstraintDefinition(const ConstraintDefinition &definition, std::string label) {
			const auto entityKind = definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
											? LogicalGraphConstraintEntityKind::LGCEK_NODE
											: LogicalGraphConstraintEntityKind::LGCEK_EDGE;
			LogicalGraphConstraintKind kind;
			switch (definition.kind) {
				case ConstraintDefinitionKind::CDK_UNIQUE:
					kind = LogicalGraphConstraintKind::LGCK_UNIQUE;
					break;
				case ConstraintDefinitionKind::CDK_NOT_NULL:
					kind = LogicalGraphConstraintKind::LGCK_NOT_NULL;
					break;
				case ConstraintDefinitionKind::CDK_PROPERTY_TYPE:
					kind = LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE;
					break;
				case ConstraintDefinitionKind::CDK_NODE_KEY:
					kind = LogicalGraphConstraintKind::LGCK_NODE_KEY;
					break;
				default:
					throw FormatException(FormatError::FERR_INVALID_ENUM,
										  "relocatable constraint catalog contains an unknown kind");
			}
			LogicalGraphConstraintPropertyType propertyType;
			switch (definition.property_type) {
				case ConstraintDefinitionPropertyType::CDPT_NONE:
					propertyType = LogicalGraphConstraintPropertyType::LGCPT_NONE;
					break;
				case ConstraintDefinitionPropertyType::CDPT_BOOLEAN:
					propertyType = LogicalGraphConstraintPropertyType::LGCPT_BOOLEAN;
					break;
				case ConstraintDefinitionPropertyType::CDPT_INTEGER:
					propertyType = LogicalGraphConstraintPropertyType::LGCPT_INTEGER;
					break;
				case ConstraintDefinitionPropertyType::CDPT_FLOAT:
					propertyType = LogicalGraphConstraintPropertyType::LGCPT_FLOAT;
					break;
				case ConstraintDefinitionPropertyType::CDPT_STRING:
					propertyType = LogicalGraphConstraintPropertyType::LGCPT_STRING;
					break;
				case ConstraintDefinitionPropertyType::CDPT_LIST:
					propertyType = LogicalGraphConstraintPropertyType::LGCPT_LIST;
					break;
				case ConstraintDefinitionPropertyType::CDPT_MAP:
					propertyType = LogicalGraphConstraintPropertyType::LGCPT_MAP;
					break;
				default:
					throw FormatException(FormatError::FERR_INVALID_ENUM,
										  "relocatable constraint catalog contains an unknown Property type");
			}
			return {definition.constraint_id, definition.name,			 entityKind,  kind,
					std::move(label),		  definition.property_names, propertyType};
		}

		struct ResolvedLogicalProperties {
			std::vector<PropertyCollectionRootEntry> entries;
		};

		template<typename ResolvePropertyKey>
		[[nodiscard]] ResolvedLogicalProperties
		resolveLogicalProperties(const std::vector<LogicalGraphWriteProperty> &properties,
								 ResolvePropertyKey &&resolvePropertyKey) {
			std::vector<LogicalGraphWriteProperty> ordered = properties;
			std::ranges::sort(ordered, {}, &LogicalGraphWriteProperty::key);
			for (size_t index = 0U; index < ordered.size(); ++index) {
				if (ordered[index].key.empty() || (index != 0U && ordered[index - 1U].key == ordered[index].key)) {
					throw std::invalid_argument(
							"logical graph creation requires nonempty, unique Property keys per entity");
				}
			}

			ResolvedLogicalProperties result;
			result.entries.reserve(ordered.size());
			for (const LogicalGraphWriteProperty &property: ordered) {
				result.entries.push_back({resolvePropertyKey(property.key), property.value});
			}
			std::ranges::sort(result.entries, {}, &PropertyCollectionRootEntry::key_token);
			for (size_t index = 1U; index < result.entries.size(); ++index) {
				if (result.entries[index - 1U].key_token == result.entries[index].key_token) {
					throw std::logic_error("logical graph creation resolved distinct Property keys to one token");
				}
			}
			return result;
		}

		template<typename ResolveSchemaToken>
		[[nodiscard]] std::vector<uint64_t> resolveLogicalLabels(const std::vector<std::string> &labels,
																 ResolveSchemaToken &&resolveSchemaToken) {
			std::vector<std::string> ordered = labels;
			std::ranges::sort(ordered);
			for (size_t index = 0U; index < ordered.size(); ++index) {
				if (ordered[index].empty()) {
					throw std::invalid_argument("logical graph creation requires nonempty label names");
				}
				if (index != 0U && ordered[index - 1U] == ordered[index]) {
					ordered.erase(ordered.begin() + static_cast<std::ptrdiff_t>(index));
					--index;
				}
			}

			std::vector<uint64_t> tokens;
			tokens.reserve(ordered.size());
			for (const std::string &label: ordered) {
				tokens.push_back(resolveSchemaToken(label));
			}
			std::ranges::sort(tokens);
			if (std::ranges::adjacent_find(tokens) != tokens.end()) {
				throw std::logic_error("logical graph creation resolved distinct labels to one Token");
			}
			return tokens;
		}

		[[nodiscard]] bool requiresConstraintSupportTree(const ConstraintDefinition &definition) {
			return definition.kind == ConstraintDefinitionKind::CDK_UNIQUE ||
				   definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY;
		}

	} // namespace

	RelocatableStorageBackend::RelocatableStorageBackend(RelocatableStorageBackendOptions options) :
		options_(std::move(options)) {}

	std::shared_ptr<RelocatableStorageBackend>
	RelocatableStorageBackend::initializeNew(RelocatableStorageBackendOptions options) {
		if (options.open_mode != OpenMode::OPEN_CREATE_NEW_FILE &&
			options.open_mode != OpenMode::OPEN_CREATE_OR_OPEN_FILE) {
			throw std::invalid_argument("relocatable native initialization requires a create-capable open mode");
		}
		validateBufferPoolCapacity(options.buffer_pool_bytes);
		const std::filesystem::path dataPath = canonicalNewPath(options.data_path, "database");

		std::unique_ptr<OwnedRelocatableBackendFile> data;
		bool created = false;
		try {
			data = OwnedRelocatableBackendFile::create(dataPath, options.buffer_pool_bytes);
			created = true;
			Superblock initial;
			initial.database_id = DatabaseId(generateDatabaseId());
			initial.generation = Generation(1U);
			initial.checkpoint_lsn = LogSequenceNumber(1U);
			initial.flags = 0U;
			static_cast<void>(SuperblockCodec::encode(initial));
			auto metadata = RelocatableMetadataStore::initializeNewComplete(data->io(), initial);
			// Do not expose a pre-bootstrap file: creation converges the initial
			// Catalog ownership and typed route/header in one durable transaction.
			metadata.reset();
			data->io()->sync();
			data.reset();
			// Bootstrap private new-file membership roots before returning a handle.
			// Existing files are never scanned or upgraded implicitly on open.
			auto bootstrapOptions = options;
			bootstrapOptions.data_path = dataPath;
			bootstrapOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
			bootstrapOptions.maintenance.reset();
			RelocatableStorageBackend bootstrapBackend(std::move(bootstrapOptions));
			bootstrapBackend.open();
			static_cast<void>(bootstrapBackend.ensureLogicalMembershipIndexes());
			bootstrapBackend.rebuildLogicalTokenCardinalityStatistics();
			bootstrapBackend.close();
		} catch (...) {
			const bool removeData = created;
			data.reset();
			std::error_code ignored;
			if (removeData) {
				static_cast<void>(std::filesystem::remove(dataPath, ignored));
			}
			throw;
		}
		data.reset();

		options.data_path = std::filesystem::canonical(dataPath);
		options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		return std::make_shared<RelocatableStorageBackend>(std::move(options));
	}

	RelocatableStorageBackend::~RelocatableStorageBackend() { releaseResources(); }

	void RelocatableStorageBackend::open() {
		if (isOpen()) {
			return;
		}
		if (options_.open_mode != OpenMode::OPEN_EXISTING_FILE &&
			options_.open_mode != OpenMode::OPEN_READ_ONLY_EXISTING_FILE) {
			throw std::invalid_argument("relocatable backend opens only existing complete databases");
		}
		if (options_.maintenance.has_value() && isReadOnly()) {
			throw std::invalid_argument("relocatable background maintenance requires a writable backend");
		}
		validateBufferPoolCapacity(options_.buffer_pool_bytes);

		const std::filesystem::path dataPath = canonicalExistingPath(options_.data_path, "database");

		// Keep every later reopen on the exact identity checked for regular-file semantics.
		options_.data_path = dataPath;

		const bool writable = !isReadOnly();
		auto data = OwnedRelocatableBackendFile::open(dataPath, writable, options_.buffer_pool_bytes);
		if (!StorageFormatProbe::inspect(*data->io()).isRelocatable()) {
			throw std::runtime_error("relocatable backend refuses a non-complete relocatable database file");
		}
		static_cast<void>(SuperblockFile(data->io()).recover());
		auto metadata = writable ? RelocatableMetadataStore::open(data->io())
								 : RelocatableMetadataStore::openReadOnly(data->io());

		try {
			data_file_ = std::move(data);
			metadata_ = std::move(metadata);
			physical_ = std::make_unique<PhysicalSegmentAllocator>(data_file_->io(), *metadata_);
			if (writable) {
				// A live session's scratch has no graph route by design, so recovering it
				// without the session lease would incorrectly treat durable live work as
				// an orphan. A later writable reopen performs recovery after its holder
				// commits, rolls back, or crashes and the OS lock is released.
				if (auto scratchRecoveryLease = tryAcquireNativeBulkSessionLease(); scratchRecoveryLease != nullptr) {
					MetadataSnapshotPin scratchSnapshot = metadata_->openSnapshot();
					if (scratchSnapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
						throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
										  "relocatable bulk scratch recovery cannot advance the checkpoint LSN");
					}
					const LogSequenceNumber scratchRecoveryLsn(scratchSnapshot.superblock().checkpoint_lsn.value() + 1U);
					scratchSnapshot.release();
					const NativeBulkRunStoreOptions scratchOptions{PhysicalSlotId(1U),
															   PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
															   RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 64U};
					NativeBulkRunStore(*physical_, *metadata_, scratchOptions).reclaimUnpublishedOnOpen(scratchRecoveryLsn);
					// Reclaim any allocator-free suffix left by an interrupted metadata or
					// segment publication after authoritative Catalog recovery.
					const RelocatableTailTruncationResult tail = metadata_->truncateReleasedTail();
					if (tail.status == RelocatableTailTruncationStatus::VTTS_IO_ERROR) {
						throw std::runtime_error(
								"relocatable native bulk staging recovery could not truncate its free tail");
					}
				}
			}
			vector_codebooks_ = std::make_unique<VectorCodebookStore>(*physical_, *metadata_,
															options_.maximum_vector_codebook_cache_bytes,
															options_.maximum_vector_codebook_cache_entries);
			// A normal open follows the published roots and must not scan every data
			// pool. A writable allocator reconciles its selected pool before frame
			// reuse; routed reads still authenticate each current owner header.
			const SharedPagePoolOpenMode poolOpenMode = isReadOnly()
																? SharedPagePoolOpenMode::SPPOM_READ_ONLY
																: SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION;
			slotted_pages_ = std::make_unique<SlottedPagePool>(data_file_->io(), *metadata_, poolOpenMode);
			btree_pages_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
			system_catalog_ = std::make_unique<SystemCatalog>(*btree_pages_, *metadata_);
			large_values_ = std::make_unique<LargeValueStore>(data_file_->io(), *metadata_, poolOpenMode);
			// The native graph path owns Properties by (EntityKind, EntityId). Do
			// not inject a retired Property-ID reader into this boundary.
			properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *large_values_, *metadata_);
			vector_pages_ = std::make_unique<VectorPageStore>(data_file_->io(), *metadata_, poolOpenMode);
			vector_manifests_ = std::make_unique<VectorManifestStore>(data_file_->io(), *metadata_, poolOpenMode);
			native_entities_ = std::make_unique<EntityPageStore>(data_file_->io(), *metadata_, poolOpenMode);
			native_labels_ =
					std::make_unique<EntityLabelSetStore>(data_file_->io(), *metadata_, *slotted_pages_, poolOpenMode);
			native_graph_reads_ = std::make_unique<PageBackedGraphReadAdapter>(
					*metadata_, *native_entities_, *native_labels_, *properties_, *btree_pages_);
			native_logical_graph_reads_ = std::make_unique<PageBackedLogicalGraphReadAdapter>(
					*native_graph_reads_, static_cast<const PageBackedGraphNameResolver &>(*this));
			if (!isReadOnly()) {
				if (options_.maximum_index_definitions == 0U ||
					options_.maximum_index_definitions == std::numeric_limits<size_t>::max() ||
					options_.maximum_constraint_definitions == 0U ||
					options_.maximum_constraint_definitions == std::numeric_limits<size_t>::max() ||
					options_.maximum_online_index_entries == 0U ||
					options_.maximum_online_index_entries == std::numeric_limits<size_t>::max() ||
					options_.maximum_online_index_scan_nodes == 0U ||
					options_.maximum_online_index_scan_nodes == std::numeric_limits<size_t>::max() ||
					options_.maximum_online_index_pages == 0U ||
					options_.maximum_online_index_pages == std::numeric_limits<size_t>::max() ||
					options_.online_index_sort_run_bytes == 0U ||
					options_.online_index_sort_run_bytes == std::numeric_limits<size_t>::max() ||
					options_.maximum_online_index_sort_runs < 2U ||
					options_.maximum_online_index_sort_runs == std::numeric_limits<size_t>::max() ||
					options_.vector_delta_rebuild_threshold == 0U ||
					options_.vector_delta_rebuild_threshold == std::numeric_limits<size_t>::max() ||
					options_.vector_delta_rebuild_threshold > std::numeric_limits<uint32_t>::max() ||
					options_.maximum_vector_delta_overlay_entries == 0U ||
					options_.maximum_vector_delta_overlay_entries == std::numeric_limits<size_t>::max() ||
					options_.maximum_vector_delta_overlay_entries > std::numeric_limits<uint32_t>::max() ||
					options_.vector_delta_rebuild_threshold > options_.maximum_vector_delta_overlay_entries ||
					options_.maximum_vector_delta_pages == 0U ||
					options_.maximum_vector_delta_pages == std::numeric_limits<size_t>::max() ||
					options_.maximum_vector_delta_overlay_entries >
							VectorDeltaMapping::maximumEntriesForPageBudget(options_.maximum_vector_delta_pages) ||
					options_.maximum_vector_training_records == 0U ||
					options_.maximum_vector_training_records == std::numeric_limits<size_t>::max() ||
					options_.maximum_vector_training_pages == 0U ||
					options_.maximum_vector_training_pages == std::numeric_limits<size_t>::max() ||
					options_.maximum_vector_training_samples == 0U ||
					options_.maximum_vector_training_samples == std::numeric_limits<size_t>::max() ||
					options_.vector_training_iterations == 0U ||
					options_.maximum_adjacency_read_edges == 0U ||
					options_.maximum_adjacency_read_edges == std::numeric_limits<size_t>::max() ||
					options_.maximum_bulk_edge_transaction_records == 0U ||
					options_.maximum_bulk_edge_transaction_records == std::numeric_limits<size_t>::max() ||
					options_.bulk_edge_id_lease_block_records == 0U ||
					options_.bulk_edge_id_lease_block_records == std::numeric_limits<size_t>::max() ||
					options_.maximum_bulk_edge_append_records == 0U ||
					options_.maximum_bulk_edge_append_records == std::numeric_limits<size_t>::max() ||
					options_.maximum_bulk_edge_append_bytes == 0U ||
					options_.maximum_bulk_edge_append_bytes == std::numeric_limits<size_t>::max() ||
					options_.bulk_edge_sort_run_bytes == 0U ||
					options_.bulk_edge_sort_run_bytes == std::numeric_limits<size_t>::max() ||
					options_.maximum_bulk_edge_sort_runs < 2U ||
					options_.maximum_bulk_edge_sort_runs == std::numeric_limits<size_t>::max() ||
					options_.maximum_bulk_edge_candidate_pages == 0U ||
					options_.maximum_bulk_edge_candidate_pages == std::numeric_limits<size_t>::max() ||
					options_.maximum_bulk_edge_index_entries == 0U ||
					options_.maximum_bulk_edge_index_entries == std::numeric_limits<size_t>::max() ||
					options_.maximum_bulk_edge_transaction_records > options_.maximum_bulk_edge_index_entries / 2U ||
					options_.maximum_vector_adjacency_exact_records == 0U ||
					options_.maximum_vector_adjacency_exact_records == std::numeric_limits<size_t>::max() ||
					options_.maximum_vector_adjacency_degree == 0U ||
					options_.maximum_vector_adjacency_degree > RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS ||
					options_.vector_adjacency_candidate_search_budget == 0U ||
					options_.vector_adjacency_candidate_search_budget == std::numeric_limits<size_t>::max() ||
					!std::isfinite(options_.vector_adjacency_pruning_alpha) ||
					options_.vector_adjacency_pruning_alpha < 1.0F ||
					options_.maximum_constraint_support_collisions == 0U ||
					options_.maximum_constraint_support_collisions == std::numeric_limits<size_t>::max()) {
					throw std::invalid_argument(
							"relocatable writable backend requires finite positive index, scan, bulk, "
							"constraint, Vector, adjacency, and collision bounds");
				}
				NativeGraphWritePlannerOptions graphWriteOptions;
				graphWriteOptions.maximum_adjacency_read_edges = options_.maximum_adjacency_read_edges;
				native_graph_write_planner_ = std::make_unique<NativeGraphWritePlanner>(
						*metadata_, *native_entities_, *native_labels_, *properties_, *btree_pages_, graphWriteOptions);
				const NativeGraphMutationExecutorOptions nativeMutationOptions{
						options_.maximum_index_definitions,		 options_.maximum_constraint_definitions,
						options_.maximum_online_index_entries,	 options_.maximum_constraint_support_collisions,
						options_.maximum_online_index_entries,	 options_.maximum_online_index_pages,
						options_.vector_delta_rebuild_threshold, options_.maximum_vector_delta_overlay_entries,
						options_.maximum_vector_delta_pages};
				native_graph_mutation_executor_ = std::make_unique<NativeGraphMutationExecutor>(
						*native_graph_write_planner_, *native_graph_reads_, *btree_pages_, *vector_pages_,
						*vector_manifests_, *metadata_, NativeGraphMutationTokenResolvers{},
						[this](const MetadataSnapshotPin &snapshot) {
							return readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
						},
						[this](const MetadataSnapshotPin &snapshot) {
							return readAllConstraintDefinitions(snapshot, options_.maximum_constraint_definitions);
						},
						nativeMutationOptions, system_catalog_.get());
			}
			if (options_.verify_integrity_on_open) {
				static_cast<void>(verifyIntegrity());
			}
			requires_reopen_after_indeterminate_write_.store(false, std::memory_order_release);
			initializeNativeMaintenanceService();
		} catch (...) {
			releaseResources();
			throw;
		}
	}

	void RelocatableStorageBackend::initializeNativeMaintenanceService() {
		if (!options_.maintenance.has_value()) {
			return;
		}
		maintenance_ = std::make_unique<RelocatableMaintenanceService>(
				*slotted_pages_, *btree_pages_, *vector_pages_, *vector_manifests_, *native_entities_,
				*vector_codebooks_, *large_values_, *physical_, *metadata_, native_publication_mutex_,
				*options_.maintenance,
				[this](std::exception_ptr) {
					requires_reopen_after_indeterminate_write_.store(true, std::memory_order_release);
				},
				[this] { return !requires_reopen_after_indeterminate_write_.load(std::memory_order_acquire); },
				[this](uint64_t indexId) {
					MetadataSnapshotPin snapshot = metadata_->openSnapshot();
					const std::vector<IndexDefinition> definitions =
							readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
					const auto definition =
							std::ranges::find_if(definitions, [indexId](const IndexDefinition &candidate) {
								return candidate.index_id == indexId;
							});
					if (definition == definitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR) {
						snapshot.release();
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable Vector maintenance request names an unknown Vector Index");
					}
					const std::string indexName = definition->name;
					snapshot.release();
					static_cast<void>(
							trainLogicalVectorUnderPublicationLock({indexName, options_.maximum_vector_training_records,
																	options_.maximum_vector_training_pages}));
					return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 0U, 0U};
				},
				[this](uint64_t afterIndexId, size_t maximumMetadataPages) {
					RelocatableVectorRebuildDebtScan result;
					MetadataSnapshotPin snapshot = metadata_->openSnapshot();
					std::set<uint64_t> knownVectorIndexes;
					for (const IndexDefinition &definition:
						 readAllIndexDefinitions(snapshot, options_.maximum_index_definitions)) {
						if (definition.kind == IndexDefinitionKind::IDK_VECTOR) {
							knownVectorIndexes.insert(definition.index_id);
						}
					}
					size_t visitedMetadataPages = 0U;
					result.exhausted = metadata_->visitVectorManifestLocationsAfter(
							snapshot, afterIndexId, maximumMetadataPages,
							[this, &snapshot, &knownVectorIndexes, &result](const VectorManifestLocation &location) {
								if (!knownVectorIndexes.contains(location.index_id)) {
									throw FormatException(
											FormatError::FERR_INVALID_STATE,
											"relocatable Vector Manifest has no matching Vector Index definition");
								}
								const VectorManifest manifest =
										vector_manifests_->read(snapshot, location.index_id).manifest();
								result.last_scanned_index_id = location.index_id;
								if (manifest.delta_entry_count > options_.vector_delta_rebuild_threshold) {
									result.index_id = location.index_id;
									return false;
								}
								return true;
							},
							&visitedMetadataPages);
					result.scanned_metadata_pages = visitedMetadataPages;
					snapshot.release();
					return result;
				},
				[this]() -> std::optional<RelocatableGlobalAdjacencyPoolReclaimCandidate> {
					MetadataSnapshotPin snapshot = metadata_->openSnapshot();
					if (metadata_->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value()) {
						snapshot.release();
						return std::nullopt;
					}
					std::optional<RelocatableGlobalAdjacencyPoolReclaimCandidate> result;
					metadata_->visitCatalogEntries(snapshot, [this, &snapshot, &result](const CatalogEntry &pool) {
						if (result.has_value() || pool.owner_id != RSS_GLOBAL_ADJACENCY_INDEX_ID ||
							pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL ||
							(pool.state != SegmentState::SGS_SEALED && pool.state != SegmentState::SGS_RETIRED) ||
							!metadata_->isPhysicalSlotAllocated(snapshot, pool.physical_slot) ||
							!metadata_->findPageLocationsForSegment(snapshot, pool.segment_id).empty()) {
							return;
						}
						const RelocatableGlobalAdjacencyPoolReclaimState state =
								pool.state == SegmentState::SGS_SEALED
										? RelocatableGlobalAdjacencyPoolReclaimState::RGAPRS_SEALED
										: RelocatableGlobalAdjacencyPoolReclaimState::RGAPRS_RETIRED;
						result = {pool.segment_id, pool.generation, state};
					});
					snapshot.release();
					return result;
				});
		// Validate every persisted Vector route before a worker can inspect it.
		MetadataSnapshotPin maintenanceSnapshot = metadata_->openSnapshot();
		std::set<uint64_t> knownVectorIndexes;
		for (const IndexDefinition &definition:
			 readAllIndexDefinitions(maintenanceSnapshot, options_.maximum_index_definitions)) {
			if (definition.kind == IndexDefinitionKind::IDK_VECTOR) {
				knownVectorIndexes.insert(definition.index_id);
			}
		}
		metadata_->visitVectorManifestLocations(
				maintenanceSnapshot, [&knownVectorIndexes](const VectorManifestLocation &location) {
					if (!knownVectorIndexes.contains(location.index_id)) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable Vector Manifest has no matching Vector Index definition");
					}
				});
		maintenanceSnapshot.release();
		const std::optional<PropertyValue> persistedMaintenanceEnabled =
				findConfiguration("storage.compaction.enabled");
		bool maintenanceEnabled = true;
		if (persistedMaintenanceEnabled.has_value()) {
			if (persistedMaintenanceEnabled->getType() != PropertyType::BOOLEAN) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable maintenance configuration must be a Boolean");
			}
			maintenanceEnabled = std::get<bool>(persistedMaintenanceEnabled->getVariant());
		}
		static_cast<void>(maintenance_->enqueueVectorRebuildRecovery());
		// Keep the existing one-slot Vector recovery configuration valid. With
		// more capacity, admit the independent adjacency sweep immediately; a
		// committed graph write also retries admission after durable publication.
		if (options_.maintenance->loop.maximum_outstanding_tasks > 1U) {
			static_cast<void>(maintenance_->enqueueGlobalAdjacencyPoolReclamation());
		}
		maintenance_->start();
		if (!maintenanceEnabled) {
			maintenance_->pause();
		}
	}

	void RelocatableStorageBackend::close() {
		if (!isOpen()) {
			return;
		}
		const bool maintenanceWasRunning =
				maintenance_ != nullptr &&
				maintenance_->metrics().loop.state == RelocatableMaintenanceLoopState::VMLS_RUNNING;
		bool maintenancePausedForClose = false;
		if (maintenanceWasRunning) {
			try {
				maintenance_->pause();
				maintenancePausedForClose = true;
			} catch (const std::logic_error &) {
				if (maintenance_->metrics().loop.state != RelocatableMaintenanceLoopState::VMLS_FAULTED) {
					throw;
				}
			}
		}
		if (metadata_->activeSnapshotCount() != 0U) {
			if (maintenancePausedForClose) {
				maintenance_->resume();
			}
			throw std::logic_error("relocatable backend cannot close while logical read snapshots remain pinned");
		}
		if (maintenance_ != nullptr) {
			maintenance_->stop();
		}
		if (!isReadOnly()) {
			flush();
		}
		releaseResources();
	}

	void RelocatableStorageBackend::flush() {
		requireOpen();
		if (isReadOnly()) {
			return;
		}
		data_file_->io()->sync();
	}

	RelocatableReadSnapshot RelocatableStorageBackend::openSnapshot() const {
		requireOpen();
		return RelocatableReadSnapshot(this, metadata_->openSnapshot());
	}

	LogicalGraphMutationFence RelocatableStorageBackend::makeLogicalGraphMutationFence(const Superblock &superblock) {
		if (!superblock.database_id.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical graph fence requires a valid database identity");
		}
		LogicalGraphMutationFence fence;
		fence.backend_token = superblock.database_id.value();
		fence.format_token = RSS_LOGICAL_GRAPH_FENCE_FORMAT_TOKEN;
		size_t offset = 0U;
		for (const LogicalGraphFenceRootMember member: RSS_LOGICAL_GRAPH_FENCE_ROOT_MEMBERS) {
			appendLogicalGraphRootTokens(fence, offset, superblock.*member);
		}
		if (offset != fence.graph_state_tokens.size()) {
			throw std::logic_error("relocatable logical graph fence root layout is incomplete");
		}
		return fence;
	}

	LogicalGraphSchemaIdentity RelocatableStorageBackend::makeLogicalGraphSchemaIdentity(const Superblock &superblock) {
		const auto fence = makeLogicalGraphMutationFence(superblock);
		const auto &catalog = superblock.index_root_directory_root;
		const auto &pages = superblock.page_directory_root;
		return LogicalGraphSchemaIdentity{fence.backend_token, fence.format_token,
			{catalog.physical_slot.value(), static_cast<uint64_t>(catalog.frame_index), catalog.generation.value(),
			 pages.physical_slot.value(), static_cast<uint64_t>(pages.frame_index), pages.generation.value()}, 0U, 0U};
	}

	std::optional<LogicalGraphSchemaIdentity> RelocatableStorageBackend::logicalPlanCacheIdentity() const {
		requireOpen();
		const auto pin = metadata_->openSnapshot();
		return makeLogicalGraphSchemaIdentity(pin.superblock());
	}

	void RelocatableStorageBackend::requireNoIndeterminateWrite() const {
		if (requires_reopen_after_indeterminate_write_.load(std::memory_order_acquire)) {
			throw std::logic_error("relocatable write outcome is indeterminate; close and reopen the backend before "
								   "another write");
		}
	}

	void RelocatableStorageBackend::markReopenRequiredIfPublicationAdvanced(const Superblock &before) noexcept {
		try {
			if (metadata_ == nullptr) { // initialized backend construction installs the metadata store before any write can reach this cleanup helper.
				return;
			}
			MetadataSnapshotPin after = metadata_->openSnapshot();
			const bool publicationAdvanced = after.superblock() != before;
			after.release();
			if (publicationAdvanced) {
				requires_reopen_after_indeterminate_write_.store(true, std::memory_order_release);
			}
		} catch (...) {
			// Failure to inspect the post-error root is itself indeterminate.
			requires_reopen_after_indeterminate_write_.store(true, std::memory_order_release);
		}
	}

	NativeGraphWriteCommitResult
	RelocatableStorageBackend::commitNativeGraphWriteTransaction(NativeGraphWriteTransaction &transaction) {
		const Superblock before = transaction.baseSnapshot().superblock();
		try {
			return transaction.commit();
		} catch (...) {
			markReopenRequiredIfPublicationAdvanced(before);
			throw;
		}
	}

	std::unique_ptr<LogicalGraphTransaction> RelocatableStorageBackend::beginLogicalGraphTransaction(bool readOnly) {
		requireOpen();
		if (!readOnly && isReadOnly()) {
			throw std::logic_error(
					"relocatable logical write transaction requires the native graph writer on a writable backend");
		}
		if (!readOnly) {
			requireNoIndeterminateWrite();
		}

		// The logical wrapper and the provisional entity-ID range must originate
		// from one graph generation. Metadata-only allocator publications are safe,
		// while a concurrent graph publication is retried before the transaction is
		// handed to a caller.
		constexpr size_t MAX_SNAPSHOT_RETRIES = 4U;
		for (size_t attempt = 0U; attempt < MAX_SNAPSHOT_RETRIES; ++attempt) {
			RelocatableReadSnapshot rawSnapshot = openSnapshot();
			const Superblock &superblock = rawSnapshot.superblock();
			const LogicalGraphMutationFence expectedFence = makeLogicalGraphMutationFence(superblock);
			const Generation baseMetadataGeneration = superblock.generation;
			std::unique_ptr<LogicalGraphVectorSearchSnapshot> logicalSnapshot = openLogicalGraphVectorSearchSnapshot();
			if (logicalSnapshot == nullptr) {
				throw std::logic_error("relocatable logical transaction could not construct its logical read snapshot");
			}
			const std::optional<LogicalGraphMutationFence> actualFence = logicalSnapshot->mutationFence();
			if (actualFence.has_value() && *actualFence == expectedFence) {
				rawSnapshot.release();
				return std::make_unique<RelocatableLogicalGraphTransaction>(*this, readOnly, std::move(logicalSnapshot),
																			expectedFence, baseMetadataGeneration);
			}
			logicalSnapshot.reset();
			rawSnapshot.release();
		}
		throw LogicalGraphMutationConflict(
				"relocatable logical transaction could not pin one stable graph generation before its retry limit");
	}

	NativeGraphLogicalIdReservation RelocatableStorageBackend::reserveLogicalGraphCreationIds(uint64_t nodeCount,
																							  uint64_t edgeCount) {
		requireOpen();
		if (isReadOnly() || (nodeCount == 0U && edgeCount == 0U)) {
			throw std::logic_error(
					"relocatable logical creation identity reservation requires an open writable native graph backend");
		}
		requireNoIndeterminateWrite();
		return native_graph_write_planner_->reserveLogicalIds(nodeCount, edgeCount);
	}

	NativeGraphLogicalIdReservation RelocatableStorageBackend::reserveNativeBulkEdgeSessionIds(uint64_t edgeCount) {
		requireOpen();
		if (isReadOnly() || edgeCount == 0U ||
			edgeCount > static_cast<uint64_t>(maximumNativeBulkEdgeTransactionRecords())) {
			throw std::invalid_argument(
					"relocatable streamed bulk Edge session requires bounded positive writable work");
		}
		requireNoIndeterminateWrite();
		const NativeGraphLogicalIdLease lease = metadata_->reserveNativeGraphLogicalIds(0U, edgeCount);
		return {{}, {lease.edges}};
	}

	void RelocatableStorageBackend::abandonLogicalGraphCreationIds(
			const NativeGraphLogicalIdReservation &reservation) noexcept {
		if (metadata_ != nullptr) {
			metadata_->abandonNativeGraphLogicalIdReservation(reservation);
		}
	}

	uint64_t RelocatableStorageBackend::allocateNativeBulkScratchOwner() {
		requireOpen();
		const uint64_t owner = next_bulk_scratch_owner_id_.fetch_add(1U, std::memory_order_relaxed);
		if (owner == 0U) {
			throw std::overflow_error("relocatable native bulk scratch owner ID overflowed");
		}
		return owner;
	}

	std::unique_ptr<NativeBulkSessionLease> RelocatableStorageBackend::tryAcquireNativeBulkSessionLease() {
		requireOpen();
		if (isReadOnly()) {
			throw std::logic_error("relocatable bulk session requires an open writable backend");
		}
		auto lease = std::make_unique<NativeBulkSessionLease>(
				nativeBulkSessionProcessMutex(data_file_->io()->getWriteFd()), data_file_->io()->getWriteFd());
		return lease->isHeld() ? std::move(lease) : nullptr;
	}

	std::unique_ptr<NativeBulkRunStore> RelocatableStorageBackend::createNativeBulkRunStore() {
		requireOpen();
		if (isReadOnly()) {
			throw std::logic_error("relocatable native bulk scratch requires an open writable backend");
		}
		return std::make_unique<NativeBulkRunStore>(
				*physical_, *metadata_,
				NativeBulkRunStoreOptions{PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
										  RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 64U, 8U});
	}

	Generation RelocatableStorageBackend::currentNativeMetadataGeneration() const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const Generation generation = snapshot.superblock().generation;
		snapshot.release();
		return generation;
	}

	LogicalGraphMutationFence RelocatableStorageBackend::currentNativeLogicalGraphMutationFence() const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const LogicalGraphMutationFence fence = makeLogicalGraphMutationFence(snapshot.superblock());
		snapshot.release();
		return fence;
	}

	bool RelocatableStorageBackend::quiesceNativeMaintenanceForExternalRebase() {
		if (maintenance_ == nullptr) {
			return false;
		}
		const RelocatableMaintenanceLoopState state = maintenance_->metrics().loop.state;
		if (state == RelocatableMaintenanceLoopState::VMLS_PAUSED) {
			return false;
		}
		if (state != RelocatableMaintenanceLoopState::VMLS_RUNNING) {
			throw std::logic_error(
					"relocatable maintenance must be running or paused before an external metadata rebase");
		}
		// Do this outside native_publication_mutex_: an in-flight maintenance task
		// can be waiting to acquire that same publication gate.
		maintenance_->pause();
		return true;
	}

	void RelocatableStorageBackend::refreshNativeMetadataAfterExternalPublication(const bool restoreMaintenancePaused) {
		requireOpen();
		if (isReadOnly()) {
			throw std::logic_error("relocatable native metadata refresh requires an open writable backend");
		}
		const bool reattachMaintenance = maintenance_ != nullptr;
		if (reattachMaintenance) {
			if (maintenance_->metrics().loop.state != RelocatableMaintenanceLoopState::VMLS_PAUSED) {
				throw std::logic_error(
						"relocatable native metadata refresh requires quiesced maintenance before rebasing a handle");
			}
			maintenance_->stop();
			maintenance_.reset();
		}
		data_file_->io()->discardBufferPoolForExternalRebase();
		metadata_->refreshAfterExternalPublication();

		// These components retain pool-header views derived from the old root. The
		// durable scratch store keeps only the physical allocator and metadata
		// references, so it remains valid while graph-facing views are rebound.
		native_graph_mutation_executor_.reset();
		native_logical_graph_reads_.reset();
		native_graph_reads_.reset();
		native_graph_write_planner_.reset();
		vector_manifests_.reset();
		vector_pages_.reset();
		properties_.reset();
		large_values_.reset();
		native_labels_.reset();
		native_entities_.reset();
		system_catalog_.reset();
		btree_pages_.reset();
		slotted_pages_.reset();
		vector_codebooks_.reset();

		vector_codebooks_ = std::make_unique<VectorCodebookStore>(*physical_, *metadata_,
																  options_.maximum_vector_codebook_cache_bytes,
																  options_.maximum_vector_codebook_cache_entries);
		const SharedPagePoolOpenMode poolOpenMode = SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION;
		slotted_pages_ = std::make_unique<SlottedPagePool>(data_file_->io(), *metadata_, poolOpenMode);
		btree_pages_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
		system_catalog_ = std::make_unique<SystemCatalog>(*btree_pages_, *metadata_);
		large_values_ = std::make_unique<LargeValueStore>(data_file_->io(), *metadata_, poolOpenMode);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *large_values_, *metadata_);
		vector_pages_ = std::make_unique<VectorPageStore>(data_file_->io(), *metadata_, poolOpenMode);
		vector_manifests_ = std::make_unique<VectorManifestStore>(data_file_->io(), *metadata_, poolOpenMode);
		native_entities_ = std::make_unique<EntityPageStore>(data_file_->io(), *metadata_, poolOpenMode);
		native_labels_ =
				std::make_unique<EntityLabelSetStore>(data_file_->io(), *metadata_, *slotted_pages_, poolOpenMode);
		native_graph_reads_ = std::make_unique<PageBackedGraphReadAdapter>(
				*metadata_, *native_entities_, *native_labels_, *properties_, *btree_pages_);
		native_logical_graph_reads_ = std::make_unique<PageBackedLogicalGraphReadAdapter>(
				*native_graph_reads_, static_cast<const PageBackedGraphNameResolver &>(*this));

		NativeGraphWritePlannerOptions graphWriteOptions;
		graphWriteOptions.maximum_adjacency_read_edges = options_.maximum_adjacency_read_edges;
		native_graph_write_planner_ = std::make_unique<NativeGraphWritePlanner>(
				*metadata_, *native_entities_, *native_labels_, *properties_, *btree_pages_, graphWriteOptions);
		const NativeGraphMutationExecutorOptions nativeMutationOptions{
				options_.maximum_index_definitions, options_.maximum_constraint_definitions,
				options_.maximum_online_index_entries, options_.maximum_constraint_support_collisions,
				options_.maximum_online_index_entries, options_.maximum_online_index_pages,
				options_.vector_delta_rebuild_threshold, options_.maximum_vector_delta_overlay_entries,
				options_.maximum_vector_delta_pages};
		native_graph_mutation_executor_ = std::make_unique<NativeGraphMutationExecutor>(
				*native_graph_write_planner_, *native_graph_reads_, *btree_pages_, *vector_pages_, *vector_manifests_,
				*metadata_, NativeGraphMutationTokenResolvers{},
				[this](const MetadataSnapshotPin &snapshot) {
					return readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
				},
				[this](const MetadataSnapshotPin &snapshot) {
					return readAllConstraintDefinitions(snapshot, options_.maximum_constraint_definitions);
				},
				nativeMutationOptions, system_catalog_.get());
		if (reattachMaintenance) {
			initializeNativeMaintenanceService();
			if (restoreMaintenancePaused &&
				maintenance_->metrics().loop.state == RelocatableMaintenanceLoopState::VMLS_RUNNING) {
				maintenance_->pause();
			}
		}
	}

	LogSequenceNumber RelocatableStorageBackend::nextNativeBulkScratchCheckpointLsn() const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const uint64_t checkpoint = snapshot.superblock().checkpoint_lsn.value();
		snapshot.release();
		if (checkpoint == std::numeric_limits<uint64_t>::max()) {
			throw std::overflow_error("relocatable native bulk scratch checkpoint LSN overflowed");
		}
		return LogSequenceNumber(checkpoint + 1U);
	}

	void RelocatableStorageBackend::commitNativeStreamedBulkEdges(NativeBulkRunStore &scratch,
																  NativeBulkRunFragmentLocation manifestLocation,
																  const NativeGraphLogicalIdReservation &reservation,
																  const NativeStreamedGraphPublicationPlan &plan,
																  const LogicalGraphMutationFence &expectedFence) {
		requireOpen();
		requireNoIndeterminateWrite();
		if (isReadOnly() || !manifestLocation.isValid() || reservation.edge_ranges.empty()) {
			throw std::logic_error("relocatable streamed bulk Edge commit requires an open native publication domain");
		}
		constexpr uint16_t BULK_EDGE_MANIFEST_DEPTH = 8U;
		auto publicationFence = acquireNativePublicationGate();
		MetadataSnapshotPin current = metadata_->openSnapshot();
		NativeStreamedGraphPublicationPlan publicationPlan = plan;
		if (makeLogicalGraphMutationFence(current.superblock()) != expectedFence) {
			// The sealed source owns no mutable graph state. Revalidate its endpoint
			// dependency under the publication gate, then rebuild all COW shapes from
			// the current roots so an independent adjacency publication is retained.
			const NativeBulkEdgeRecordSource source =
					nativeBulkEdgeRecordSource(scratch, manifestLocation, BULK_EDGE_MANIFEST_DEPTH);
			publicationPlan = NativeStreamedGraphPublicationPlan::derive(
					source, reservation.edge_ranges, nativeBulkEdgePublicationPreflightOptions(),
					[this, &current](uint64_t nodeId) {
						if (!metadata_->findNodePageLocationOptional(current, nodeId).has_value()) {
							throw LogicalGraphMutationConflict(
									"relocatable streamed bulk Edge commit lost a persisted endpoint during rebase");
						}
						return true;
					});
		}
		const uint64_t checkpoint = current.superblock().checkpoint_lsn.value() + 1U;
		// Private candidates are one physical segment each. The public sort-run
		// budget may be larger, but cannot widen a candidate fragment past its
		// sealed segment payload.
		const size_t candidateBytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
		const size_t contentCandidates =
				publicationPlan.large_value_count == 0U
						? 0U
						: NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
								  *large_values_, publicationPlan.large_value_count, candidateBytes,
								  options_.maximum_bulk_edge_sort_runs)
							  .candidate_segment_count;
		const size_t referenceMapCandidates =
				publicationPlan.large_value_count == 0U
						? 0U
						: NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
								  publicationPlan.large_value_count, candidateBytes, options_.maximum_bulk_edge_sort_runs)
								  .candidate_segment_count;
		NativeStreamedGraphFinalPublicationRequest request;
		request.expected_metadata_generation = current.superblock().generation;
		request.edge_id_reservation = reservation;
		request.durable_source = {&scratch, manifestLocation, 8U};
		request.publication_plan = publicationPlan;
		request.candidate_layout = NativeStreamedGraphCandidateLayout::derive(
				publicationPlan, contentCandidates, referenceMapCandidates);
		// Export only scalar admission facts. The sealed source and private segment
		// identities remain inaccessible to the calibration process.
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.candidate_capacity_segments",
									   request.candidate_layout.candidate_count);
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.candidate_limit",
									   publicationPlan.maximum_candidate_segments);
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.token_candidates",
									   publicationPlan.token_candidate_segments);
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.token_resolver_candidates",
									   publicationPlan.token_resolver_candidate_segments);
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.outgoing_adjacency_candidates",
									   publicationPlan.outgoing_adjacency_candidate_segments);
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.incoming_adjacency_candidates",
									   publicationPlan.incoming_adjacency_candidate_segments);
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.large_value_content_candidates", contentCandidates);
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.large_value_reference_map_candidates",
								   referenceMapCandidates);
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.source_run_count",
									   publicationPlan.source_run_count);
		traceNativeBulkCandidateMetric("native_graph.streamed.scratch.merge_passes", publicationPlan.merge_passes);
		const PhysicalSlotId first(1U), last(std::numeric_limits<uint64_t>::max());
		request.private_candidate_plan = {SegmentKind::SGK_BULK_SCRATCH,
										  allocateNativeBulkScratchOwner(),
										  first,
										  last,
										  RSS_ENVELOPE_BYTES,
										  {},
										  request.candidate_layout.candidate_count};
		request.entity_pool_owner_id = allocateNativeBulkScratchOwner();
		request.property_pool_owner_id = allocateNativeBulkScratchOwner();
		request.first_entity_output_slot = first;
		request.last_entity_output_slot = last;
		request.first_property_output_slot = first;
		request.last_property_output_slot = last;
		request.first_large_value_data_output_slot = first;
		request.last_large_value_data_output_slot = last;
		request.first_large_value_manifest_output_slot = first;
		request.last_large_value_manifest_output_slot = last;
		request.preparation = {
				{options_.maximum_bulk_edge_transaction_records,
				 std::max<size_t>(1U, publicationPlan.token_entry_count),
				 candidateBytes,
				 {options_.maximum_bulk_edge_sort_runs, candidateBytes, options_.maximum_bulk_edge_candidate_pages, 8U,
				  NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}},
				options_.maximum_bulk_edge_candidate_pages,
				large_values_.get(),
				{},
				{candidateBytes, options_.maximum_bulk_edge_sort_runs, options_.maximum_bulk_edge_candidate_pages, 8U}};
		request.index_planning = {
				{first, last, options_.maximum_bulk_edge_index_entries, options_.maximum_bulk_edge_candidate_pages},
				{first, last, options_.maximum_bulk_edge_index_entries, options_.maximum_bulk_edge_candidate_pages, 8U},
				{first, last, options_.maximum_bulk_edge_index_entries, options_.maximum_bulk_edge_candidate_pages,
				 8U},
				{first, last, options_.maximum_bulk_edge_index_entries, options_.maximum_bulk_edge_candidate_pages,
				 options_.maximum_index_definitions, options_.online_index_sort_run_bytes, options_.maximum_online_index_sort_runs}};
		const Superblock publicationBase = current.superblock();
		current.release();
		try {
			static_cast<void>(metadata_->publishStreamedEdgeGraph(request, *system_catalog_, *btree_pages_,
																  large_values_.get(), LogSequenceNumber(checkpoint)));
		} catch (...) {
			// A failed final Superblock sync can still leave the complete candidate
			// recoverable. Fence this handle whenever inspection observes that new
			// graph generation, so callers never mistake it for a rollback.
			markReopenRequiredIfPublicationAdvanced(publicationBase);
			throw;
		}
		try {
			releaseNativeBulkEdgeManifest(scratch, manifestLocation, nextNativeBulkScratchCheckpointLsn());
		} catch (...) {
			// The graph root is already durable. Keep that commit observable and
			// require reopen recovery before another write can reuse scratch storage.
			requireReopenAfterNativeBulkStagingFailure();
		}
	}

	std::unique_lock<std::mutex> RelocatableStorageBackend::acquireNativePublicationGate() {
		requireOpen();
		if (isReadOnly()) {
			throw std::logic_error("relocatable read-only backend cannot open native bulk staging");
		}
		return std::unique_lock<std::mutex>(native_publication_mutex_);
	}

	void RelocatableStorageBackend::requireReopenAfterNativeBulkStagingFailure() noexcept {
		requires_reopen_after_indeterminate_write_.store(true, std::memory_order_release);
	}

	std::vector<uint64_t>
	RelocatableStorageBackend::createLogicalNodes(const std::vector<LogicalGraphNodeCreateRequest> &requests) {
		requireOpen();
		if (requests.empty()) {
			return {};
		}
		if (isReadOnly()) {
			throw std::logic_error(
					"relocatable logical Node creation requires the native graph writer on a writable backend");
		}
		requireNoIndeterminateWrite();
		LogicalGraphMutationBatch batch;
		batch.node_creates = requests;
		return executeLogicalGraphMutationBatch(batch).created_node_ids;
	}

	std::vector<uint64_t>
	RelocatableStorageBackend::createLogicalEdges(const std::vector<LogicalGraphEdgeCreateRequest> &requests) {
		requireOpen();
		if (requests.empty()) {
			return {};
		}
		if (isReadOnly()) {
			throw std::logic_error(
					"relocatable logical Edge creation requires the native graph writer on a writable backend");
		}
		requireNoIndeterminateWrite();
		LogicalGraphMutationBatch batch;
		batch.edge_creates.reserve(requests.size());
		for (const LogicalGraphEdgeCreateRequest &request: requests) {
			batch.edge_creates.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
										   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, request.source_node_id},
										  {LogicalGraphMutationEntityKind::LGMK_NODE,
										   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, request.target_node_id},
										  request.type,
										  request.properties});
		}
		return executeLogicalGraphMutationBatch(batch).created_edge_ids;
	}

	void RelocatableStorageBackend::replaceLogicalNodeProperties(
			const std::vector<LogicalGraphPropertyReplaceRequest> &requests) {
		requireOpen();
		if (requests.empty()) {
			return;
		}
		if (isReadOnly()) {
			throw std::logic_error("relocatable logical Node Property replacement requires the native graph writer on "
								   "a writable backend");
		}
		requireNoIndeterminateWrite();
		LogicalGraphMutationBatch batch;
		batch.property_replacements.reserve(requests.size());
		for (const LogicalGraphPropertyReplaceRequest &request: requests) {
			batch.property_replacements.push_back(
					{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
					  request.entity_id},
					 request.properties});
		}
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void RelocatableStorageBackend::replaceLogicalEdgeProperties(
			const std::vector<LogicalGraphPropertyReplaceRequest> &requests) {
		requireOpen();
		if (requests.empty()) {
			return;
		}
		if (isReadOnly()) {
			throw std::logic_error("relocatable logical Edge Property replacement requires the native graph writer on "
								   "a writable backend");
		}
		requireNoIndeterminateWrite();
		LogicalGraphMutationBatch batch;
		batch.property_replacements.reserve(requests.size());
		for (const LogicalGraphPropertyReplaceRequest &request: requests) {
			batch.property_replacements.push_back(
					{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
					  request.entity_id},
					 request.properties});
		}
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void RelocatableStorageBackend::deleteLogicalEdges(const std::vector<LogicalGraphEdgeDeleteRequest> &requests) {
		requireOpen();
		if (requests.empty()) {
			return;
		}
		if (isReadOnly()) {
			throw std::logic_error(
					"relocatable logical Edge deletion requires the native graph writer on a writable backend");
		}
		requireNoIndeterminateWrite();
		LogicalGraphMutationBatch batch;
		batch.edge_deletes = requests;
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void RelocatableStorageBackend::deleteLogicalNodes(const std::vector<LogicalGraphNodeDeleteRequest> &requests) {
		requireOpen();
		if (requests.empty()) {
			return;
		}
		if (isReadOnly()) {
			throw std::logic_error(
					"relocatable logical Node deletion requires the native graph writer on a writable backend");
		}
		requireNoIndeterminateWrite();
		LogicalGraphMutationBatch batch;
		batch.node_deletes = requests;
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	LogicalGraphMutationBatchResult RelocatableStorageBackend::executeLogicalGraphMutationBatch(
			const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence) {
		return executeLogicalGraphMutationBatchWithReadControl(batch, expectedFence, {});
	}

	LogicalGraphMutationBatchResult RelocatableStorageBackend::executeLogicalGraphMutationBatchWithReadControl(
		const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence,
		const LogicalGraphMutationReadControl &control) {
		return executeNativeMutationBatch(batch, expectedFence, control, {}, 0U);
	}

	LogicalGraphMutationBatchResult RelocatableStorageBackend::executeLogicalGraphMutationBatchWithPropertyPatches(
		LogicalGraphReadSnapshot &snapshot, const LogicalGraphMutationBatch &companionBatch,
		std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
		const LogicalGraphMutationFence &expectedFence, const LogicalGraphMutationReadControl &control) {
		if (control.checkpoint) control.checkpoint();
		if (!expectedFence.isValid() || maximumPropertyEntries == 0U)
			throw std::invalid_argument("native Property patches require a valid snapshot fence and positive entry bound");
		const auto sourceFence = snapshot.mutationFence();
		if (!sourceFence || !sourceFence->isValid())
			throw std::invalid_argument("native Property patches require a fenced source snapshot");
		if (*sourceFence != expectedFence)
			throw LogicalGraphMutationConflict("native Property patch planning fence differs from its source snapshot");
		return executeNativeMutationBatch(companionBatch, expectedFence, control, patches, maximumPropertyEntries);
	}

	LogicalGraphMutationBatchResult RelocatableStorageBackend::executeNativeMutationBatch(
		const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence,
		const LogicalGraphMutationReadControl &control,
		std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries) {
		if (control.checkpoint) control.checkpoint();
		requireOpen();
		if (batch.node_creates.empty() && batch.edge_creates.empty() && batch.property_replacements.empty() &&
			batch.node_label_replacements.empty() && batch.edge_deletes.empty() && batch.node_deletes.empty() &&
			!batch.creation_identity_plan.has_value() && patches.empty()) {
			return {};
		}
		if (isReadOnly()) {
			throw std::logic_error(
					"relocatable logical graph mutation batches require the native graph writer on a writable backend");
		}
		requireNoIndeterminateWrite();
		if (expectedFence.has_value() && !expectedFence->isValid()) {
			throw std::invalid_argument(
					"relocatable logical graph mutation batch received an invalid optimistic read fence");
		}

		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		NativeGraphMutationFenceValidator fenceValidator;
		if (expectedFence.has_value()) {
			const LogicalGraphMutationFence expected = *expectedFence;
			fenceValidator = [expected](const Superblock &base) {
				return RelocatableStorageBackend::makeLogicalGraphMutationFence(base) == expected;
			};
		}

		constexpr size_t MAX_CONFLICT_RETRIES = 4U;
		for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
			MetadataSnapshotPin beforeExecution = metadata_->openSnapshot();
			const LogicalGraphMutationFence graphFenceBeforeExecution =
					makeLogicalGraphMutationFence(beforeExecution.superblock());
			beforeExecution.release();
			NativeGraphMutationExecutionResult execution;
			try {
				execution = patches.empty()
					? native_graph_mutation_executor_->execute(batch, fenceValidator, control)
					: native_graph_mutation_executor_->executeWithPropertyPatches(
						batch, patches, maximumPropertyEntries, fenceValidator, control);
			} catch (...) {
				// A failed final durability fence can leave a complete inactive
				// Superblock selected even though the caller received an error. Compare
				// graph-state roots while this publication gate is held: metadata-only
				// ID leases may advance a Superblock generation before the Graph batch
				// starts, but only changed Graph roots make this batch indeterminate.
				try {
					MetadataSnapshotPin afterFailure = metadata_->openSnapshot();
					const bool graphStateChanged =
							makeLogicalGraphMutationFence(afterFailure.superblock()) != graphFenceBeforeExecution;
					afterFailure.release();
					if (graphStateChanged) {
						requires_reopen_after_indeterminate_write_.store(true, std::memory_order_release);
					}
				} catch (...) {
					// If recovery inspection itself fails, continuing on this handle
					// would risk writing over an unknown durable generation.
					requires_reopen_after_indeterminate_write_.store(true, std::memory_order_release);
				}
				throw;
			}
			if (execution.committed()) {
				// Queue only after the graph/Vector delta generation is durable.
				// A full transient queue cannot change the already durable commit.
				if (maintenance_ != nullptr) {
					for (const uint64_t indexId: execution.commit.vector_rebuild_index_ids) {
						try {
							static_cast<void>(maintenance_->enqueueVectorBaseRebuild(indexId));
						} catch (const std::exception &) {
							// Reopening derives the durable rebuild debt again.
						}
					}
				}
				if (maintenance_ != nullptr) {
					try {
						static_cast<void>(maintenance_->enqueueGlobalAdjacencyPoolReclamation());
					} catch (const std::exception &) {
						// A future writable open repeats the durable empty-pool scan.
					}
				}
				return execution.result;
			}
			if (expectedFence.has_value()) {
				throw LogicalGraphMutationConflict(
						"relocatable logical graph mutation batch no longer matches its read snapshot fence");
			}
		}
		throw std::runtime_error(
				"relocatable logical graph mutation batch exceeded its optimistic conflict retry limit");
	}

	LogicalGraphNode RelocatableStorageBackend::readNode(const RelocatableReadSnapshot &snapshot,
														 uint64_t nodeId) const {
		requireOwnedSnapshot(snapshot);
		return native_logical_graph_reads_->readNodeShaped(
				snapshot.pin_, nodeId, {.include_labels = true, .all_properties = true});
	}

	LogicalGraphEdge RelocatableStorageBackend::readEdge(const RelocatableReadSnapshot &snapshot,
														 uint64_t edgeId) const {
		requireOwnedSnapshot(snapshot);
		return native_logical_graph_reads_->readEdgeShaped(
				snapshot.pin_, edgeId, {.include_type = true, .all_properties = true});
	}

	bool RelocatableStorageBackend::containsLogicalNode(const RelocatableReadSnapshot &snapshot,
																																																				 uint64_t nodeId, NativeReadSession *session) const {
		requireOwnedSnapshot(snapshot);
		if (nodeId == 0U) {
			return false;
		}
		if (session != nullptr) {
			session->bind(snapshot.pin_);
			if (const auto cached = session->nodeExists(nodeId); cached.has_value()) {
				return *cached;
			}
		}
		const bool exists = metadata_->findNodePageLocationOptional(snapshot.pin_, nodeId).has_value();
		if (session != nullptr) {
			session->putNodeExists(nodeId, exists);
		}
		return exists;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeEdgeTypeCandidates(
			const MetadataSnapshotPin &snapshot, uint64_t typeToken, size_t maximumRecords,
			const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session) const {
		if (!visitor || maximumRecords == 0U) {
			throw std::invalid_argument("native type candidate access requires a visitor and positive bound");
		}
		// Historical member trees omit untyped relationships. A zero token must
		// therefore scan the primary records, never produce a false empty result.
		if (typeToken == 0U || typeToken > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
		if (session != nullptr) session->bind(snapshot);
		const auto estimate = system_catalog_->readTokenCardinalityEstimate(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, typeToken);
		if (estimate && !NativeMembershipAccessCost::preferCandidates(
				estimate->first.edge_count, estimate->second, maximumRecords, true)) return std::nullopt;
		const auto *definitions = session == nullptr ? nullptr :
				session->indexDefinitions(options_.maximum_index_definitions);
		std::vector<IndexDefinition> loaded;
		if (definitions == nullptr) {
			loaded = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) session->putIndexDefinitions(loaded);
			definitions = &loaded;
		}
		for (const auto &definition : *definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_EDGE ||
					definition.kind != IndexDefinitionKind::IDK_LABEL ||
					!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) continue;
			BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			const PropertyValue value(static_cast<int64_t>(typeToken));
			const auto lower = IndexKeyCodec::lowerBound(value);
			const auto upper = IndexKeyCodec::upperBound(value);
			const size_t cursorBound = maximumRecords == std::numeric_limits<size_t>::max() ?
					maximumRecords : maximumRecords + 1U;
			auto cursor = index.openRangeCursor(snapshot, std::span<const std::byte>(lower), cursorBound);
			uint64_t previous = 0U;
			size_t visited = 0U;
			std::vector<uint64_t> ids;
			ids.reserve(BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
			const auto emit = [&] {
				if (ids.empty()) return true;
				const auto records = native_graph_reads_->readEdgeTopologiesSorted(snapshot, ids, session);
				for (size_t i = 0U; i < ids.size(); ++i) {
					const auto &record = records[i];
					if (record.type_id != typeToken) throw FormatException(FormatError::FERR_INVALID_STATE,
							"native type Index disagrees with the primary Edge type");
					const EntityRef reference{ids[i], true, record.source_node_id, record.target_node_id, record.type_id};
					debug::PerfTrace::addValue("native_graph.read.type_index_candidates", 1);
					if (!visitor(reference)) return false;
				}
				ids.clear(); return true;
			};
			while (auto entry = cursor.next()) {
				if (entry->key > upper) break;
				if (entry->key.size() != lower.size() ||
						!std::equal(lower.begin(), lower.end() - sizeof(uint64_t), entry->key.begin())) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "native type Index has a malformed key");
				}
				const uint64_t id = IndexKeyCodec::decodeEntryEntityId(entry->key);
				if (id <= previous || entry->value_or_child != id) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "native type Index has an invalid Edge ID");
				}
				if (visited == maximumRecords) {
					if (!emit()) return false;
					throw std::runtime_error("native type candidate access exceeded its record bound");
				}
				++visited; previous = id; ids.push_back(id);
				if ((ids.size() == BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES || visited == maximumRecords) && !emit()) return false;
			}
			return emit();
		}
		return std::nullopt;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeNodeLabelCandidates(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			size_t maximumRecords, const std::function<bool(const EntityRef &, bool)> &visitor,
			NativeReadSession *session) const {
		if (!visitor || maximumRecords == 0U) {
			throw std::invalid_argument("native label candidate access requires a visitor and positive bound");
		}
		if (labels.empty()) return std::nullopt;
		if (session != nullptr) session->bind(snapshot);
		std::optional<uint64_t> token;
		std::string_view primaryLabel = labels.front();
		std::optional<uint64_t> smallestCount;
		const bool estimateDriver = labels.size() > 1U && system_catalog_->hasCompleteTokenCardinality(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL);
		for (const auto &label : labels) {
			if (label.empty()) return true;
			std::optional<uint64_t> resolved;
			const auto cached = session == nullptr ? std::nullopt : session->nodeLabelToken(label);
			if (cached.has_value()) resolved = *cached;
			else {
				resolved = system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label);
				if (session != nullptr) session->putNodeLabelToken(label, resolved);
			}
			if (!resolved.has_value()) return true;
			if (!token.has_value()) token = resolved;
			if (estimateDriver) {
				const auto count = system_catalog_->readTokenCardinality(
						snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *resolved);
				if (count && (!smallestCount || *count < *smallestCount)) {
					smallestCount = count; token = resolved; primaryLabel = label;
				}
			}
		}
		if (*token > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
		const auto estimate = system_catalog_->readTokenCardinalityEstimate(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *token);
		if (estimate && !NativeMembershipAccessCost::preferCandidates(
				estimate->first.node_count, estimate->second, maximumRecords, false)) return std::nullopt;
		const auto *definitions = session == nullptr ? nullptr :
				session->indexDefinitions(options_.maximum_index_definitions);
		std::vector<IndexDefinition> loaded;
		if (definitions == nullptr) {
			loaded = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) session->putIndexDefinitions(loaded);
			definitions = &loaded;
		}
		for (const auto &definition : *definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
					definition.kind != IndexDefinitionKind::IDK_LABEL ||
					!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) continue;
			BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			const PropertyValue value(static_cast<int64_t>(*token));
			const auto lower = IndexKeyCodec::lowerBound(value);
			const auto upper = IndexKeyCodec::upperBound(value);
			const size_t cursorBound = maximumRecords == std::numeric_limits<size_t>::max() ?
					maximumRecords : maximumRecords + 1U;
			auto cursor = index.openRangeCursor(snapshot, std::span<const std::byte>(lower), cursorBound);
			std::vector<uint64_t> ids;
			ids.reserve(BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
			uint64_t previous = 0U;
			size_t visited = 0U;
			const auto emit = [&]() {
				if (ids.empty()) return true;
				const auto primary = native_logical_graph_reads_->readNodeLabelMatchesSorted(
						snapshot, ids, {std::string(primaryLabel)}, session);
				const auto matches = labels.size() == 1U ? primary :
						native_logical_graph_reads_->readNodeLabelMatchesSorted(snapshot, ids, labels, session);
				for (size_t i = 0U; i < ids.size(); ++i) {
					if (!primary[i]) throw FormatException(FormatError::FERR_INVALID_STATE,
							"native label Index disagrees with the primary Node label set");
					// The label batch has already authenticated the primary Entity record.
					const EntityRef reference{ids[i], false, 0U, 0U, 0U};
					debug::PerfTrace::addValue("native_graph.read.label_index_candidates", 1);
					if (!visitor(reference, matches[i])) return false;
				}
				ids.clear();
				return true;
			};
			while (auto entry = cursor.next()) {
				if (entry->key > upper) break;
				if (entry->key.size() != lower.size() ||
						!std::equal(lower.begin(), lower.end() - sizeof(uint64_t), entry->key.begin())) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "native label Index has a malformed key");
				}
				const uint64_t id = IndexKeyCodec::decodeEntryEntityId(entry->key);
				if (id <= previous || entry->value_or_child != id) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "native label Index has an invalid Node ID");
				}
				if (visited == maximumRecords) {
					// Give a stopping visitor a chance to terminate at the bound.
					if (!emit()) return false;
					throw std::runtime_error("native label candidate access exceeded its record bound");
				}
				++visited; previous = id; ids.push_back(id);
				if ((ids.size() == BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES || visited == maximumRecords) && !emit()) return false;
			}
			return emit();
		}
		return std::nullopt;
	}

	std::vector<bool> RelocatableStorageBackend::containsLogicalNodesSorted(
			const RelocatableReadSnapshot &snapshot, const std::span<const uint64_t> nodeIds,
			NativeReadSession *session) const {
		requireOwnedSnapshot(snapshot);
		std::vector<bool> result(nodeIds.size(), false);
		if (nodeIds.empty()) {
			return result;
		}
		uint64_t previousNodeId = 0U;
		for (const uint64_t nodeId : nodeIds) {
			if (nodeId == 0U || nodeId <= previousNodeId) {
				throw std::invalid_argument(
						"relocatable sorted Node existence read requires strictly increasing Node IDs");
			}
			previousNodeId = nodeId;
		}
		if (session != nullptr) {
			session->bind(snapshot.pin_);
		}
		std::vector<uint64_t> missingIds;
		std::vector<size_t> missingOrdinals;
		missingIds.reserve(nodeIds.size());
		missingOrdinals.reserve(nodeIds.size());
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			if (session != nullptr) {
				if (const auto cached = session->nodeExists(nodeIds[index]); cached.has_value()) {
					result[index] = *cached;
					continue;
				}
			}
			missingIds.push_back(nodeIds[index]);
			missingOrdinals.push_back(index);
		}
		if (missingIds.empty()) {
			return result;
		}
		debug::PerfTrace::addValue("native_graph.read.node_existence_batch_requests", 1);
		debug::PerfTrace::addValue("native_graph.read.node_existence_batch_ids",
				static_cast<int64_t>(missingIds.size()));
		std::vector<std::optional<EntityPageLocation>> locations(missingIds.size());
		metadata_->findNodePageLocationsSorted(snapshot.pin_, missingIds, locations);
		for (size_t index = 0U; index < missingIds.size(); ++index) {
			result[missingOrdinals[index]] = locations[index].has_value();
			if (session != nullptr) {
				session->putNodeExists(missingIds[index], result[missingOrdinals[index]]);
			}
		}
		return result;
	}

	std::optional<IndexCandidateBatch> RelocatableStorageBackend::findNativeNodeCandidatesByExactProperties(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
			size_t maximumPropertyEntries, NativeReadSession *session, IndexCandidateBatchVisitor *visitor,
			bool *visitorCompleted, size_t maximumCandidateBytes) const {
		if (visitorCompleted != nullptr) {
			*visitorCompleted = true;
		}
		if (!snapshot.isValid() || maximumNodes == 0U || maximumPropertyEntries == 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable native exact Node lookup requires an active snapshot and positive bounds");
		}
		if (labels.size() != 1U || labels.front().empty() || properties.empty()) {
			return std::nullopt;
		}
		if (session != nullptr) {
			session->bind(snapshot);
		}
		const auto resolveLabelToken = [&]() {
			if (session != nullptr) {
				if (const auto cached = session->nodeLabelToken(labels.front()); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.label_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.index_candidate_label_token_resolution", 1);
			const std::optional<uint64_t> resolved =
					system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, labels.front());
			if (session != nullptr) {
				session->putNodeLabelToken(labels.front(), resolved);
			}
			return resolved;
		};
		const auto resolvePropertyToken = [&](const std::string &name) {
			if (session != nullptr) {
				if (const auto cached = session->propertyToken(name); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.property_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.index_candidate_property_token_resolution", 1);
			const std::optional<uint32_t> resolved = system_catalog_->findPropertyToken(snapshot, name);
			if (session != nullptr) {
				session->putPropertyToken(name, resolved);
			}
			return resolved;
		};
		std::map<std::string, const PropertyValue *> predicateValues;
		for (const auto &[name, value]: properties) {
			if (name.empty() || value.getType() == PropertyType::NULL_TYPE ||
				!predicateValues.emplace(name, &value).second) {
				return std::nullopt;
			}
		}
		const std::optional<uint64_t> labelToken = resolveLabelToken();
		if (!labelToken.has_value()) {
			IndexCandidateBatch empty;
			empty.snapshot_generation = snapshot.superblock().generation.value();
			empty.exact_match_verified = true;
			empty.label_scope_verified = true;
			return empty;
		}
		if (options_.maximum_constraint_support_collisions == 0U ||
			options_.maximum_constraint_support_collisions == std::numeric_limits<size_t>::max()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable native exact Node lookup requires a finite support-tree collision bound");
		}
		const std::vector<ConstraintDefinition> *cachedConstraints =
				session == nullptr ? nullptr : session->constraintDefinitions(options_.maximum_constraint_definitions);
		std::vector<ConstraintDefinition> loadedConstraints;
		if (cachedConstraints == nullptr) {
			debug::PerfTrace::addValue("native_graph.read.index_candidate_constraint_definition_read", 1);
			loadedConstraints = readAllConstraintDefinitions(snapshot, options_.maximum_constraint_definitions);
			if (session != nullptr) {
				session->putConstraintDefinitions(loadedConstraints);
				cachedConstraints = session->constraintDefinitions(options_.maximum_constraint_definitions);
			}
		}
		const std::vector<ConstraintDefinition> &definitions =
				cachedConstraints == nullptr ? loadedConstraints : *cachedConstraints;
		for (const ConstraintDefinition &definition: definitions) {
			const bool supportsExactLookup = definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE &&
											 (definition.kind == ConstraintDefinitionKind::CDK_UNIQUE ||
											  definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY) &&
											 definition.schema_token_id == *labelToken;
			if (!supportsExactLookup || definition.property_names.size() != predicateValues.size()) {
				continue;
			}
			std::set<std::string> definitionNames(definition.property_names.begin(), definition.property_names.end());
			bool samePropertyNames = definitionNames.size() == definition.property_names.size();
			auto definitionName = definitionNames.begin();
			auto predicateName = predicateValues.begin();
			while (samePropertyNames && definitionName != definitionNames.end()) {
				if (predicateName == predicateValues.end() || *definitionName != predicateName->first) {
					samePropertyNames = false;
					break;
				}
				++definitionName;
				++predicateName;
			}
			if (!samePropertyNames || predicateName != predicateValues.end()) {
				continue;
			}

			std::vector<PropertyValue> requestedValues;
			std::vector<uint32_t> propertyTokens;
			requestedValues.reserve(definition.property_names.size());
			propertyTokens.reserve(definition.property_names.size());
			for (const std::string &name: definition.property_names) {
				const std::optional<uint32_t> token = resolvePropertyToken(name);
				if (!token.has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup found an unknown constrained Property key");
				}
				requestedValues.push_back(*predicateValues.at(name));
				propertyTokens.push_back(*token);
			}
			const std::vector<std::byte> requestedKey = ConstraintSupportTree::canonicalKey(requestedValues);
			if (!metadata_->findIndexRootOptional(snapshot, definition.constraint_id).has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable native exact Node lookup found a unique constraint without a support tree");
			}
			const BTreeIndex support(definition.constraint_id, *btree_pages_, *metadata_);
			const std::vector<BTreePageEntry> candidates =
					support.scan(snapshot, ConstraintSupportTree::lowerBound(requestedKey),
								 checkedBoundPlusOne(options_.maximum_constraint_support_collisions,
										"relocatable native exact Node lookup collision bound overflowed"));
			size_t collisionCount = 0U;
			IndexCandidateBatch result;
			result.snapshot_generation = snapshot.superblock().generation.value();
			result.exact_match_verified = true;
			result.predicates_verified = true;
			result.label_scope_verified = true;
			for (const std::string &name: definition.property_names) {
				result.verified_property_columns.push_back({name, {}});
			}
			std::vector<uint64_t> candidateNodeIds;
			candidateNodeIds.reserve(candidates.size());
			for (const BTreePageEntry &candidate: candidates) {
				if (!ConstraintSupportTree::matchesDigest(candidate.key, requestedKey)) {
					break;
				}
				if (++collisionCount > options_.maximum_constraint_support_collisions) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup exceeded its support-tree collision bound");
				}
				const uint64_t nodeId = ConstraintSupportTree::entryEntityId(candidate.key);
				if (candidate.value_or_child != nodeId) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup found a support-tree entry with a mismatched value");
				}
				candidateNodeIds.push_back(nodeId);
			}
			if (candidateNodeIds.size() > maximumNodes) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native exact Node lookup exceeded its caller-supplied result bound");
			}
			if (!candidateNodeIds.empty()) {
				std::ranges::sort(candidateNodeIds);
				if (std::ranges::adjacent_find(candidateNodeIds) != candidateNodeIds.end()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable native exact Node lookup found duplicate support-tree Entity IDs");
				}
				// The support tree is ordered by digest then Entity ID. Validate the
				// complete digest collision batch through one sorted Entity/Property
				// pipeline instead of repeating a Directory descent per candidate.
				// The batch API retains owner, label, Property count, token, and value
				// checks; this changes only their granularity.
				prepareExactCandidateColumns(result, candidateNodeIds.size(), requestedValues, maximumCandidateBytes);
				size_t sourceBytes = candidateBatchBytes(result);
				size_t sourceBuffers = 0U;
				const auto admission = exactCandidateSourceAdmission(sourceBytes, sourceBuffers, maximumCandidateBytes);
				const auto bufferAdmission = exactCandidateBufferAdmission(sourceBytes, sourceBuffers, maximumCandidateBytes);
				native_graph_reads_->validateNodeExactPropertiesSorted(
						snapshot, candidateNodeIds, definition.schema_token_id, propertyTokens, requestedValues,
						maximumPropertyEntries, session, &admission, &bufferAdmission);
				traceNativeCandidateMetric("native_graph.read.index_candidate_verified_rows", candidateNodeIds.size());
				for (const uint64_t nodeId: candidateNodeIds) {
					result.candidates.push_back({nodeId, false, 0U, 0U, 0U});
					for (size_t propertyIndex = 0U; propertyIndex < definition.property_names.size(); ++propertyIndex) {
						result.verified_property_columns[propertyIndex].values.push_back(requestedValues[propertyIndex]);
					}
				}
			}
			if (result.candidates.size() > 1U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable native exact Node lookup found duplicate unique-constraint entries");
			}
			if (visitor != nullptr) {
				// Unique-constraint candidates use the support tree path rather than
				// the ordinary Index cursor, so enforce the same producer-side byte
				// budget before exposing this batch to a storage-neutral consumer.
				enforceCandidateBatchBytes(result, maximumCandidateBytes);
				if (!(*visitor)(result) && visitorCompleted != nullptr) {
					*visitorCompleted = false;
				}
				return IndexCandidateBatch{};
			}
			return result;
		}

		const std::vector<IndexDefinition> *cachedIndexes =
				session == nullptr ? nullptr : session->indexDefinitions(options_.maximum_index_definitions);
		std::vector<IndexDefinition> loadedIndexes;
		if (cachedIndexes == nullptr) {
			debug::PerfTrace::addValue("native_graph.read.index_candidate_definition_read", 1);
			loadedIndexes = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) {
				session->putIndexDefinitions(loadedIndexes);
				cachedIndexes = session->indexDefinitions(options_.maximum_index_definitions);
			}
		}
		const std::vector<IndexDefinition> &indexDefinitions =
				cachedIndexes == nullptr ? loadedIndexes : *cachedIndexes;
		for (const IndexDefinition &definition: indexDefinitions) {
			const bool supportsExactLookup = definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE &&
											 (definition.kind == IndexDefinitionKind::IDK_PROPERTY ||
											  definition.kind == IndexDefinitionKind::IDK_COMPOSITE) &&
											 definition.label_token_id == *labelToken &&
											 definition.property_names.size() == predicateValues.size();
			if (!supportsExactLookup) {
				continue;
			}
			std::set<std::string> definitionNames(definition.property_names.begin(), definition.property_names.end());
			bool samePropertyNames = definitionNames.size() == definition.property_names.size();
			auto definitionName = definitionNames.begin();
			auto predicateName = predicateValues.begin();
			while (samePropertyNames && definitionName != definitionNames.end()) {
				if (predicateName == predicateValues.end() || *definitionName != predicateName->first) {
					samePropertyNames = false;
					break;
				}
				++definitionName;
				++predicateName;
			}
			if (!samePropertyNames || predicateName != predicateValues.end()) {
				continue;
			}

			std::vector<PropertyValue> requestedValues;
			std::vector<uint32_t> propertyTokens;
			requestedValues.reserve(definition.property_names.size());
			propertyTokens.reserve(definition.property_names.size());
			bool indexable = true;
			for (const std::string &name: definition.property_names) {
				const PropertyValue &value = *predicateValues.at(name);
				const PropertyType type = value.getType();
				if ((type != PropertyType::BOOLEAN && type != PropertyType::INTEGER && type != PropertyType::DOUBLE &&
					 type != PropertyType::STRING) ||
					(type == PropertyType::DOUBLE && !std::isfinite(std::get<double>(value.getVariant())))) {
					indexable = false;
					break;
				}
				const std::optional<uint32_t> token = resolvePropertyToken(name);
				if (!token.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable native exact Node lookup found an unknown indexed Property key");
				}
				requestedValues.push_back(value);
				propertyTokens.push_back(*token);
			}
			if (!indexable) {
				// The logical predicate is valid, but this bounded ordered index codec
				// deliberately omits non-scalar and non-finite values.
				return std::nullopt;
			}
			const std::optional<std::vector<std::byte>> entryKey =
					definition.kind == IndexDefinitionKind::IDK_PROPERTY
							? IndexKeyCodec::tryEncodeEntryKey(requestedValues.front(), 1U)
							: IndexKeyCodec::tryEncodeCompositeEntryKey(requestedValues, 1U);
			if (!entryKey.has_value()) {
				return std::nullopt;
			}
			std::vector<std::byte> valuePrefix = *entryKey;
			valuePrefix.resize(valuePrefix.size() - sizeof(uint64_t));
			if (!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable native exact Node lookup found an Index definition without a B+Tree root");
			}
			if (maximumNodes == std::numeric_limits<size_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable native exact Node lookup cannot extend its result bound");
			}
			const BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			if (visitor != nullptr) {
				size_t batchCapacity = candidateBatchCapacity(maximumNodes, maximumCandidateBytes);
				size_t keyBytes = 0U;
				for (const auto &name: definition.property_names) {
					if (name.capacity() > std::numeric_limits<size_t>::max() - keyBytes) {
						throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable native index candidate byte accounting overflowed");
					}
					keyBytes += name.capacity();
				}
				batchCapacity = std::min(batchCapacity, std::max<size_t>(1U,
					(maximumCandidateBytes - std::min(maximumCandidateBytes, keyBytes)) / exactCandidateRowBytes(requestedValues)));
				auto cursor = index.openRangeCursor(snapshot, std::optional(std::span<const std::byte>(*entryKey)),
						options_.maximum_online_index_entries, BufferPoolReadHint::BPRH_SEQUENTIAL);
				debug::PerfTrace::addValue("native_graph.read.index_candidate_root_descent", 1);
				std::vector<BTreePageEntry> pending;
				pending.reserve(batchCapacity);
				size_t totalCandidates = 0U;
				const auto emit = [&]() -> bool {
					if (pending.empty()) {
						return true;
					}
					IndexCandidateBatch batch;
					batch.snapshot_generation = snapshot.superblock().generation.value();
					batch.exact_match_verified = true;
					batch.predicates_verified = true;
					batch.label_scope_verified = true;
					for (const std::string &name: definition.property_names) {
						batch.verified_property_columns.push_back({name, {}});
					}
					std::vector<uint64_t> nodeIds;
					nodeIds.reserve(pending.size());
					uint64_t previousNodeId = 0U;
					for (const BTreePageEntry &candidate: pending) {
						if (candidate.key.size() != valuePrefix.size() + sizeof(uint64_t) ||
								candidate.key.size() < valuePrefix.size() ||
								!std::equal(valuePrefix.begin(), valuePrefix.end(), candidate.key.begin())) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native exact Node lookup found a malformed scalar Index key");
						}
						const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate.key);
						if (nodeId <= previousNodeId || candidate.value_or_child != nodeId) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native exact Node lookup found a noncanonical scalar Index entry");
						}
						previousNodeId = nodeId;
						nodeIds.push_back(nodeId);
					}
					prepareExactCandidateColumns(batch, nodeIds.size(), requestedValues, maximumCandidateBytes);
					size_t sourceBytes = candidateBatchBytes(batch);
					size_t sourceBuffers = 0U;
					const auto admission = exactCandidateSourceAdmission(sourceBytes, sourceBuffers, maximumCandidateBytes);
					const auto bufferAdmission = exactCandidateBufferAdmission(sourceBytes, sourceBuffers, maximumCandidateBytes);
					native_graph_reads_->validateNodeExactPropertiesSorted(snapshot, nodeIds, definition.label_token_id,
							propertyTokens, requestedValues, maximumPropertyEntries, session, &admission, &bufferAdmission);
					traceNativeCandidateMetric("native_graph.read.index_candidate_verified_rows", nodeIds.size());
					for (const uint64_t nodeId: nodeIds) {
						batch.candidates.push_back({nodeId, false, 0U, 0U, 0U});
						for (size_t propertyIndex = 0U; propertyIndex < definition.property_names.size(); ++propertyIndex) {
							batch.verified_property_columns[propertyIndex].values.push_back(requestedValues[propertyIndex]);
						}
					}
					pending.clear();
					traceNativeCandidateMetric("native_graph.read.index_candidate_batches", 1U);
					traceNativeCandidateMetric("native_graph.read.index_candidate_emitted_rows", batch.candidates.size());
					enforceCandidateBatchBytes(batch, maximumCandidateBytes);
					return (*visitor)(batch);
				};
			for (;;) {
				const std::optional<BTreePageEntry> candidate = cursor.next();
				if (!candidate.has_value() || candidate->key.size() < valuePrefix.size() ||
						!std::equal(valuePrefix.begin(), valuePrefix.end(), candidate->key.begin())) {
					break;
				}
				if (++totalCandidates > maximumNodes) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup exceeded its caller-supplied result bound");
				}
				pending.push_back(*candidate);
				if (pending.size() == batchCapacity && !emit()) {
					if (visitorCompleted != nullptr) {
						*visitorCompleted = false;
					}
					return IndexCandidateBatch{};
				}
			}
			if (!emit() && visitorCompleted != nullptr) {
				*visitorCompleted = false;
			}
			// The stream path deliberately performs no unbounded result collection.
			return IndexCandidateBatch{};
		}
			const std::vector<BTreePageEntry> candidates = index.scan(snapshot, *entryKey,
						checkedBoundPlusOne(maximumNodes, "relocatable native exact Node lookup result bound overflowed"));
			IndexCandidateBatch result;
			result.snapshot_generation = snapshot.superblock().generation.value();
			result.exact_match_verified = true;
			result.predicates_verified = true;
			result.label_scope_verified = true;
			for (const std::string &name: definition.property_names) {
				result.verified_property_columns.push_back({name, {}});
			}
			uint64_t previousNodeId = 0U;
			for (const BTreePageEntry &candidate: candidates) {
				if (candidate.key.size() < valuePrefix.size() ||
					!std::equal(valuePrefix.begin(), valuePrefix.end(), candidate.key.begin())) {
					break;
				}
				if (candidate.key.size() != valuePrefix.size() + sizeof(uint64_t)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable native exact Node lookup found a malformed scalar Index key");
				}
				const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate.key);
				if (nodeId <= previousNodeId || candidate.value_or_child != nodeId) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup found a noncanonical scalar Index entry");
				}
				previousNodeId = nodeId;
				if (result.candidates.size() == maximumNodes) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup exceeded its caller-supplied result bound");
				}
				result.candidates.push_back({nodeId, false, 0U, 0U, 0U});
				for (size_t propertyIndex = 0U; propertyIndex < definition.property_names.size(); ++propertyIndex) {
					result.verified_property_columns[propertyIndex].values.push_back(requestedValues[propertyIndex]);
				}
			}
			if (!result.candidates.empty()) {
				std::vector<uint64_t> nodeIds;
				nodeIds.reserve(result.candidates.size());
				for (const EntityRef &candidate: result.candidates) {
					nodeIds.push_back(candidate.entity_id);
				}
				// A B+Tree page proves only its own checksum and key ordering. Verify
				// the authoritative Property collection once for the complete sorted
				// candidate batch so valid-but-stale index keys fail closed.
				native_graph_reads_->validateNodeExactPropertiesSorted(
						snapshot, nodeIds, definition.label_token_id, propertyTokens, requestedValues,
						maximumPropertyEntries, session);
			}
			return result;
		}
		return std::nullopt;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeNodeCandidatesByExactProperties(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, const size_t maximumNodes,
			const size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor,
			NativeReadSession *session, size_t maximumCandidateBytes) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable native exact Node candidate stream requires a visitor");
		}
		bool completed = true;
		IndexCandidateBatchVisitor callback = visitor;
		const auto batch = findNativeNodeCandidatesByExactProperties(snapshot, labels, properties, maximumNodes,
				maximumPropertyEntries, session, &callback, &completed, maximumCandidateBytes);
		if (!batch.has_value()) {
			return std::nullopt;
		}
		return completed;
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNativeNodesByExactProperties(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, const size_t maximumNodes,
			const size_t maximumPropertyEntries, NativeReadSession *session) const {
		const std::optional<IndexCandidateBatch> candidates =
				findNativeNodeCandidatesByExactProperties(snapshot, labels, properties, maximumNodes,
																				 maximumPropertyEntries, session);
		if (!candidates.has_value()) {
			return std::nullopt;
		}
		if (!candidates->exact_match_verified || candidates->candidates.size() > maximumNodes) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable native exact Node lookup returned an invalid candidate batch");
		}
		std::vector<LogicalGraphNode> result;
		std::vector<uint64_t> nodeIds;
		nodeIds.reserve(candidates->candidates.size());
		for (const EntityRef &candidate: candidates->candidates) {
			if (candidate.entity_id == 0U || candidate.is_edge) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native exact Node lookup returned an invalid Node reference");
			}
			nodeIds.push_back(candidate.entity_id);
		}
		// Candidate verification already returns a strict logical-ID order. Read the
		// complete records through one page-grouped batch so the public result does
		// not repeat a Directory descent for every indexed Node.
		return native_logical_graph_reads_->readNodesShapedSorted(
				snapshot, nodeIds, {.include_labels = true, .all_properties = true}, session);
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNodesByExactProperties(
			const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
			size_t maximumPropertyEntries) const {
		requireOwnedSnapshot(snapshot);
		return findNativeNodesByExactProperties(snapshot.pin_, labels, properties, maximumNodes,
																				maximumPropertyEntries);
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNativeNodesByPropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries,
			NativeReadSession *session, IndexCandidateBatch *candidateOutput,
			IndexCandidateBatchVisitor *visitor, bool *visitorCompleted, size_t maximumCandidateBytes) const {
		if (visitorCompleted != nullptr) {
			*visitorCompleted = true;
		}
		if (candidateOutput != nullptr) {
			candidateOutput->candidates.clear();
			candidateOutput->verified_property_columns.clear();
			candidateOutput->snapshot_generation = snapshot.isValid() ? snapshot.superblock().generation.value() : 0U;
			candidateOutput->exact_match_verified = false;
			candidateOutput->predicates_verified = false;
			candidateOutput->label_scope_verified = false;
		}
		if (!snapshot.isValid() || maximumNodes == 0U || maximumPropertyEntries == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable native range Node lookup requires an active snapshot and positive bounds");
		}
		if (candidateOutput != nullptr) {
			// Once the request shape and resource bounds are valid, an empty result
			// (for example an unknown label/property) is still an authenticated,
			// complete candidate answer rather than a fallback signal.
			candidateOutput->predicates_verified = true;
			candidateOutput->label_scope_verified = true;
		}
		if (session != nullptr) {
			session->bind(snapshot);
		}
		const auto resolveLabelToken = [&]() {
			if (session != nullptr) {
				if (const auto cached = session->nodeLabelToken(labels.front()); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.label_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.range_index_label_token_resolution", 1);
			const std::optional<uint64_t> resolved =
					system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, labels.front());
			if (session != nullptr) {
				session->putNodeLabelToken(labels.front(), resolved);
			}
			return resolved;
		};
		const auto resolvePropertyToken = [&](const std::string &name) {
			if (session != nullptr) {
				if (const auto cached = session->propertyToken(name); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.property_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.range_index_property_token_resolution", 1);
			const std::optional<uint32_t> resolved = system_catalog_->findPropertyToken(snapshot, name);
			if (session != nullptr) {
				session->putPropertyToken(name, resolved);
			}
			return resolved;
		};
		const auto isOpenBound = [](const PropertyValue &value) {
			return value.getType() != PropertyType::NULL_TYPE;
		};
		if (labels.size() != 1U || labels.front().empty() || range.key.empty() ||
			(!isOpenBound(range.min_value) && !isOpenBound(range.max_value))) {
			return std::nullopt;
		}
		const auto isIndexableBound = [isOpenBound](const PropertyValue &value) {
			if (!isOpenBound(value)) {
				return true;
			}
			const PropertyType type = value.getType();
			return (type == PropertyType::BOOLEAN || type == PropertyType::INTEGER || type == PropertyType::DOUBLE ||
					type == PropertyType::STRING) &&
				   (type != PropertyType::DOUBLE || std::isfinite(std::get<double>(value.getVariant())));
		};
		if (!isIndexableBound(range.min_value) || !isIndexableBound(range.max_value)) {
			return std::nullopt;
		}
		if (isOpenBound(range.min_value) && isOpenBound(range.max_value) &&
			(range.min_value > range.max_value ||
			 (range.min_value == range.max_value && (!range.min_inclusive || !range.max_inclusive)))) {
			return std::vector<LogicalGraphNode>{};
		}
		const std::optional<uint64_t> labelToken = resolveLabelToken();
		if (!labelToken.has_value()) {
			return std::vector<LogicalGraphNode>{};
		}
		const std::optional<uint32_t> propertyToken = resolvePropertyToken(range.key);
		if (!propertyToken.has_value()) {
			return std::vector<LogicalGraphNode>{};
		}
		if (options_.maximum_online_index_entries == 0U ||
			options_.maximum_online_index_entries == std::numeric_limits<size_t>::max()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable native range Node lookup requires a finite Index-entry bound");
		}

		const std::vector<IndexDefinition> *cachedIndexes =
				session == nullptr ? nullptr : session->indexDefinitions(options_.maximum_index_definitions);
		std::vector<IndexDefinition> loadedIndexes;
		if (cachedIndexes == nullptr) {
			debug::PerfTrace::addValue("native_graph.read.range_index_definition_read", 1);
			loadedIndexes = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) {
				session->putIndexDefinitions(loadedIndexes);
				cachedIndexes = session->indexDefinitions(options_.maximum_index_definitions);
			}
		}
		const std::vector<IndexDefinition> &definitions = cachedIndexes == nullptr ? loadedIndexes : *cachedIndexes;
		for (const IndexDefinition &definition: definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
				definition.kind != IndexDefinitionKind::IDK_PROPERTY || definition.label_token_id != *labelToken ||
				definition.property_names.size() != 1U || definition.property_names.front() != range.key) {
				continue;
			}
			if (!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native range Node lookup found an Index definition without a B+Tree root");
			}
			std::optional<std::vector<std::byte>> lower;
			if (isOpenBound(range.min_value)) {
				// Encode once, including the Entity-ID sentinel for the requested edge.
				lower = IndexKeyCodec::tryEncodeEntryKey(range.min_value,
						range.min_inclusive ? 1U : std::numeric_limits<uint64_t>::max());
				if (!lower.has_value()) return std::nullopt;
			}
			const BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			if (candidateOutput != nullptr) {
				candidateOutput->verified_property_columns.push_back({range.key, {}});
			}
			if (candidateOutput != nullptr && visitor != nullptr) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable native range Node lookup cannot collect and stream candidates simultaneously");
			}
			std::vector<LogicalGraphNode> result;
			std::vector<uint64_t> resultNodeIds;
			CandidateDedupMemoryResource dedupMemory(maximumCandidateBytes);
			std::pmr::unordered_set<uint64_t> seenNodeIds(&dedupMemory);
			// The cursor retains one verified root-to-leaf route. Stream callers use
			// bounded batches; complete callers keep the historical whole-result path
			// so their result-bound semantics remain unchanged.
			auto cursor = index.openRangeCursor(
					snapshot, lower.has_value() ? std::optional(std::span<const std::byte>(*lower)) : std::nullopt,
					options_.maximum_online_index_entries, BufferPoolReadHint::BPRH_SEQUENTIAL);
			debug::PerfTrace::addValue("native_graph.read.index_candidate_root_descent", 1);
			const std::optional<std::vector<std::byte>> upperExclusive = [&]() -> std::optional<std::vector<std::byte>> {
				if (!isOpenBound(range.max_value)) {
					return std::nullopt;
				}
				return range.max_inclusive ? std::optional(IndexKeyCodec::upperBound(range.max_value))
										 : std::optional(IndexKeyCodec::encodeEntryKey(range.max_value, 1U));
			}();
		// The default cursor batch is intentionally small for callers that do not
		// expose a byte budget. Query execution supplies a bounded candidate-byte
		// budget, so use that budget to enlarge the I/O batch and avoid repeating
		// Directory/Property-page work for every few hundred indexed rows. The
		// conservative per-row estimate keeps temporary staging bounded; the final
		// authenticated batch byte check remains authoritative for variable values.
		const size_t batchCapacity = candidateBatchCapacity(maximumNodes, maximumCandidateBytes);
		std::vector<BTreePageEntry> pending;
		if (visitor != nullptr) {
			pending.reserve(batchCapacity);
		}
		size_t boundaryKeyBytes = 0U;
		const std::function<void(size_t)> admitBoundaryKey = [&](size_t bytes) {
			size_t retained = dedupMemory.retainedBytes();
			const auto add = [&](size_t count, size_t width) {
				if (count > (maximumCandidateBytes - retained) / width) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "range cursor boundary exceeded its byte bound");
				}
				retained += count * width;
			};
			add(pending.capacity(), sizeof(BTreePageEntry));
			for (const auto &entry: pending) add(entry.key.capacity(), 1U);
			add(bytes, 1U);
			boundaryKeyBytes = bytes;
		};
		dedupMemory.setAllocationAdmission([&](size_t bytes) {
			const size_t retainedBoundary = boundaryKeyBytes;
			if (bytes > maximumCandidateBytes - retainedBoundary) {
				throw FormatException(FormatError::FERR_INVALID_STATE, "range dedup allocation exceeded its shared byte bound");
			}
			admitBoundaryKey(retainedBoundary + bytes);
			boundaryKeyBytes = retainedBoundary;
		});
		size_t totalCandidates = 0U;
		struct RangeSourceBytePressure {};
		const auto emitOnce = [&](const std::span<const BTreePageEntry> entries) -> bool {
			if (entries.empty()) {
				return true;
			}
			size_t candidateStagingBytes = dedupMemory.retainedBytes();
			if (boundaryKeyBytes > maximumCandidateBytes - candidateStagingBytes) throw RangeSourceBytePressure{};
			candidateStagingBytes += boundaryKeyBytes;
			// The whole pending batch stays alive during recursive splits.
			// Charge its slots and owned keys, not only the emitted subspan.
			if (pending.capacity() > (maximumCandidateBytes - candidateStagingBytes) / sizeof(BTreePageEntry))
				throw RangeSourceBytePressure{};
			candidateStagingBytes += pending.capacity() * sizeof(BTreePageEntry);
			for (const auto &entry: pending) {
				if (entry.key.capacity() > maximumCandidateBytes - candidateStagingBytes) throw RangeSourceBytePressure{};
				candidateStagingBytes += entry.key.capacity();
			}
			const auto checkSlots = [&](size_t count, size_t width) {
				if (count > (maximumCandidateBytes - candidateStagingBytes) / width)
					throw RangeSourceBytePressure{};
			};
			checkSlots(entries.size(), sizeof(uint64_t));
			std::vector<uint64_t> nodeIds;
			nodeIds.reserve(entries.size());
			checkSlots(nodeIds.capacity(), sizeof(uint64_t));
			candidateStagingBytes += nodeIds.capacity() * sizeof(uint64_t);
			for (const BTreePageEntry &candidate: entries) {
				if (candidate.key.size() <= sizeof(uint64_t)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup found a malformed scalar Index key");
				}
				const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate.key);
				if (candidate.value_or_child != nodeId) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup found a noncanonical scalar Index entry");
				}
				nodeIds.push_back(nodeId);
			}
			checkSlots(nodeIds.size(), sizeof(size_t));
			std::vector<size_t> propertyOrder(nodeIds.size());
			checkSlots(propertyOrder.capacity(), sizeof(size_t));
			candidateStagingBytes += propertyOrder.capacity() * sizeof(size_t);
			std::iota(propertyOrder.begin(), propertyOrder.end(), 0U);
			std::ranges::sort(propertyOrder, [&nodeIds](const size_t left, const size_t right) {
				return nodeIds[left] < nodeIds[right];
			});
			std::vector<uint64_t> sortedNodeIds;
			checkSlots(nodeIds.size(), sizeof(uint64_t));
			sortedNodeIds.reserve(nodeIds.size());
			checkSlots(sortedNodeIds.capacity(), sizeof(uint64_t));
			candidateStagingBytes += sortedNodeIds.capacity() * sizeof(uint64_t);
			for (const size_t index: propertyOrder) {
				sortedNodeIds.push_back(nodeIds[index]);
			}
			const std::array<uint64_t, 1U> labelTokens{definition.label_token_id};
			size_t labelBufferBytes = 0U;
			const SelectedPropertyReadAdmission admitLabelBuffers = [&](size_t bytes) {
				if (bytes > maximumCandidateBytes - candidateStagingBytes) throw RangeSourceBytePressure{};
				labelBufferBytes = bytes;
			};
			const std::vector<bool> labelMatches =
					native_graph_reads_->readNodeLabelMatchesSorted(snapshot, sortedNodeIds, labelTokens, session, &admitLabelBuffers);
			if (labelMatches.size() != sortedNodeIds.size() ||
				std::ranges::any_of(labelMatches, [](bool matches) { return !matches; })) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable native range Node lookup found an invalid indexed label");
			}
			size_t selectedSourceBytes = candidateStagingBytes;
			size_t largeValueBufferBytes = labelBufferBytes;
			const SelectedPropertyReadAdmission admission = [&](size_t bytes) {
				if (bytes > maximumCandidateBytes - selectedSourceBytes ||
					largeValueBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes)
					throw RangeSourceBytePressure{};
				selectedSourceBytes += bytes;
				traceNativeCandidateMetric("native_graph.read.range_candidate_source_bytes", bytes);
			};
			const SelectedPropertyReadAdmission admitLargeValueBuffers = [&](size_t bytes) {
				if (bytes > maximumCandidateBytes - selectedSourceBytes ||
					labelBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes) throw RangeSourceBytePressure{};
				largeValueBufferBytes = labelBufferBytes + bytes;
			};
			auto propertyColumnsAndCounts = native_graph_reads_->readNodePropertyColumnsAndCountsSorted(
					snapshot, sortedNodeIds, std::array<uint32_t, 1U>{*propertyToken}, session, &admission, &admitLargeValueBuffers);
			auto &propertyColumns = propertyColumnsAndCounts.first;
			const auto &propertyCounts = propertyColumnsAndCounts.second;
			if (labelMatches.size() != sortedNodeIds.size() || propertyCounts.size() != sortedNodeIds.size() ||
					propertyColumns.size() != 1U || propertyColumns.front().size() != propertyOrder.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup returned an invalid candidate validation batch");
			}
			traceNativeCandidateMetric("native_graph.read.index_candidate_liveness_probes", sortedNodeIds.size());
			for (size_t index = 0U; index < sortedNodeIds.size(); ++index) {
				if (!labelMatches[index] || !propertyCounts[index].has_value() ||
						*propertyCounts[index] > maximumPropertyEntries) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup found an invalid indexed Node");
				}
			}
			// Scatter in place using the existing source-to-destination permutation.
			// No second array of PropertyValue slots needs to coexist with the reader output.
			auto &properties = propertyColumns.front();
			for (size_t index = 0U; index < propertyOrder.size(); ++index) {
				while (propertyOrder[index] != index) {
					const size_t destination = propertyOrder[index];
					std::swap(properties[index], properties[destination]);
					std::swap(propertyOrder[index], propertyOrder[destination]);
				}
			}
			size_t outputBufferBytes = 0U;
			const SelectedPropertyReadAdmission admitOutput = [&](size_t bytes) {
				if (bytes > maximumCandidateBytes - selectedSourceBytes ||
					largeValueBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes) throw RangeSourceBytePressure{};
				outputBufferBytes = bytes;
			};
			IndexCandidateBatch streamBatch;
			if (visitor != nullptr) {
				streamBatch.snapshot_generation = snapshot.superblock().generation.value();
				streamBatch.predicates_verified = true;
				streamBatch.label_scope_verified = true;
				reserveCandidateOutput(streamBatch, entries.size(), std::span<const std::string>(&range.key, 1U), admitOutput);
			}
			for (size_t index = 0U; index < entries.size(); ++index) {
				const BTreePageEntry &candidate = entries[index];
				const uint64_t nodeId = nodeIds[index];
				std::optional<PropertyValue> &property = properties[index];
				if (!property.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup found a stale scalar Index entry");
				}
				const std::optional<std::vector<std::byte>> expected = IndexKeyCodec::tryEncodeEntryKey(*property, nodeId);
				if (!expected.has_value() || candidate.key != *expected) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup found a noncanonical scalar Index entry");
				}
				const bool belowLower = isOpenBound(range.min_value) &&
						(range.min_inclusive ? *property < range.min_value : *property <= range.min_value);
				if (belowLower) {
					continue;
				}
					if (candidateOutput != nullptr) {
						candidateOutput->candidates.push_back({nodeId, false, 0U, 0U, 0U});
						candidateOutput->verified_property_columns.front().values.push_back(std::move(*property));
						traceNativeCandidateMetric("native_graph.read.index_candidate_verified_rows", 1U);
				} else if (visitor != nullptr) {
					streamBatch.candidates.push_back({nodeId, false, 0U, 0U, 0U});
					streamBatch.verified_property_columns.front().values.push_back(std::move(*property));
				} else {
					resultNodeIds.push_back(nodeId);
				}
			}
			if (visitor == nullptr) {
				return true;
			}
			// Range index order is value-major. Sort only this bounded batch so the
			// storage-neutral runtime can validate its monotonic candidate contract.
			// Reuse the already admitted row permutation for output ordering.
			propertyOrder.resize(streamBatch.candidates.size());
			auto &order = propertyOrder;
			std::iota(order.begin(), order.end(), 0U);
			std::ranges::sort(order, [&streamBatch](const size_t left, const size_t right) {
				return streamBatch.candidates[left].entity_id < streamBatch.candidates[right].entity_id;
			});
			reorderCandidateRowsInPlace(streamBatch, order);
			traceNativeCandidateMetric("native_graph.read.index_candidate_batches", streamBatch.candidates.empty() ? 0U : 1U);
			traceNativeCandidateMetric("native_graph.read.index_candidate_emitted_rows", streamBatch.candidates.size());
			enforceCandidateBatchBytes(streamBatch, maximumCandidateBytes);
			return streamBatch.candidates.empty() || (*visitor)(streamBatch);
		};
		std::function<bool(std::span<const BTreePageEntry>)> emit;
		emit = [&](std::span<const BTreePageEntry> entries) -> bool {
			try {
				return emitOnce(entries);
			} catch (const RangeSourceBytePressure &) {
				if (entries.size() <= 1U) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable native index candidate batch exceeded its byte bound");
				}
			}
			traceNativeCandidateMetric("native_graph.read.range_candidate_source_splits", 1U);
			const size_t left = entries.size() / 2U;
			return emit(entries.first(left)) && emit(entries.subspan(left));
		};

		for (;;) {
			std::optional<BTreePageEntry> candidate = cursor.next(admitBoundaryKey);
			if (!candidate.has_value()) {
				break;
			}
			if (upperExclusive.has_value() &&
					(range.max_inclusive ? std::lexicographical_compare(upperExclusive->begin(), upperExclusive->end(),
							candidate->key.begin(), candidate->key.end())
							: !std::lexicographical_compare(candidate->key.begin(), candidate->key.end(),
																 upperExclusive->begin(), upperExclusive->end()))) {
				break;
			}
			if (candidate->key.size() <= sizeof(uint64_t)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup found a malformed scalar Index key");
			}
			const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate->key);
			if (candidate->value_or_child != nodeId || !seenNodeIds.insert(nodeId).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup found a noncanonical scalar Index entry");
			}
			++totalCandidates;
			if (totalCandidates > maximumNodes) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup exceeded its caller-supplied result bound");
			}
			if (visitor != nullptr) {
				pending.push_back(std::move(*candidate));
				if (pending.size() == batchCapacity) {
					if (!emit(std::span<const BTreePageEntry>(pending))) {
						if (visitorCompleted != nullptr) {
							*visitorCompleted = false;
						}
						return std::vector<LogicalGraphNode>{};
					}
					pending.clear();
				}
			} else {
				// Complete callers retain a bounded vector to preserve their explicit
				// maximumNodes contract and deterministic final Node-ID ordering.
				// Defer Property validation until all candidates are sorted by owner.
				// This path is handled by the same emitter after the cursor is drained.
				pending.push_back(std::move(*candidate));
			}
		}
		if (visitor != nullptr) {
			if (!pending.empty() && !emit(std::span<const BTreePageEntry>(pending)) && visitorCompleted != nullptr) {
				*visitorCompleted = false;
			}
			return std::vector<LogicalGraphNode>{};
		}
		// Complete callers now validate their collected candidates in one owner-
		// sorted batch, then reuse the emitter's normal result/candidate-output path.
		if (!pending.empty()) {
			if (!emit(std::span<const BTreePageEntry>(pending))) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup unexpectedly stopped a complete validation");
			}
		}
		if (visitor == nullptr && candidateOutput == nullptr && !resultNodeIds.empty()) {
			std::ranges::sort(resultNodeIds);
			result = native_logical_graph_reads_->readNodesShapedSorted(
					snapshot, resultNodeIds, {.include_labels = true, .all_properties = true}, session);
		}
		std::ranges::sort(result, {}, &LogicalGraphNode::node_id);
		if (candidateOutput != nullptr) {
			std::vector<size_t> order(candidateOutput->candidates.size());
			std::iota(order.begin(), order.end(), 0U);
			std::ranges::sort(order, [candidateOutput](size_t left, size_t right) {
				return candidateOutput->candidates[left].entity_id < candidateOutput->candidates[right].entity_id;
			});
			reorderCandidateRowsInPlace(*candidateOutput, order);
		}
		return result;
		}
		return std::nullopt;
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNodesByPropertyRange(
			const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries) const {
		requireOwnedSnapshot(snapshot);
		return findNativeNodesByPropertyRange(snapshot.pin_, labels, range, maximumNodes, maximumPropertyEntries);
	}

	std::optional<IndexCandidateBatch> RelocatableStorageBackend::findNativeNodeCandidatesByPropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries,
			NativeReadSession *session) const {
		IndexCandidateBatch result;
		const auto complete = findNativeNodesByPropertyRange(snapshot, labels, range, maximumNodes, maximumPropertyEntries,
												 session, &result);
		if (!complete.has_value()) {
			return std::nullopt;
		}
		if (!result.predicates_verified || result.candidates.size() > maximumNodes) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
										"relocatable native range Node lookup returned an invalid candidate batch");
		}
		for (const auto &column: result.verified_property_columns) {
			if (column.values.size() != result.candidates.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
										"relocatable native range Node lookup returned an invalid Property column");
			}
		}
		// The native source owns ordering; validate its contract without another
		// permutation array or a second copy of every verified Property column.
		if (!std::ranges::is_sorted(result.candidates, {}, &EntityRef::entity_id)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable native range Node lookup returned an unsorted candidate batch");
		}
		return result;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeNodeCandidatesByPropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphPropertyRange &range, const size_t maximumNodes, const size_t maximumPropertyEntries,
			const IndexCandidateBatchVisitor &visitor, NativeReadSession *session, size_t maximumCandidateBytes) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable native range Node candidate stream requires a visitor");
		}
		bool completed = true;
		IndexCandidateBatchVisitor callback = visitor;
		const auto result = findNativeNodesByPropertyRange(snapshot, labels, range, maximumNodes,
				maximumPropertyEntries, session, nullptr, &callback, &completed, maximumCandidateBytes);
		if (!result.has_value()) {
			return std::nullopt;
		}
		return completed;
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNativeNodesByCompositePropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphCompositePropertyRange &request, size_t maximumNodes,
			size_t maximumPropertyEntries, NativeReadSession *session, IndexCandidateBatch *candidateOutput,
			IndexCandidateBatchVisitor *visitor, bool *visitorCompleted, size_t maximumCandidateBytes) const {
		if (visitorCompleted != nullptr) {
			*visitorCompleted = true;
		}
		if (candidateOutput != nullptr) {
			candidateOutput->candidates.clear();
			candidateOutput->verified_property_columns.clear();
			candidateOutput->snapshot_generation = snapshot.isValid() ? snapshot.superblock().generation.value() : 0U;
			candidateOutput->exact_match_verified = false;
			candidateOutput->predicates_verified = false;
			candidateOutput->label_scope_verified = false;
		}
		if (!snapshot.isValid() || maximumNodes == 0U || maximumPropertyEntries == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable composite range Node lookup requires an active snapshot and positive bounds");
		}
		if (candidateOutput != nullptr) {
			candidateOutput->predicates_verified = true;
			candidateOutput->label_scope_verified = true;
		}
		if (session != nullptr) {
			session->bind(snapshot);
		}
		const auto resolveLabelToken = [&]() {
			if (session != nullptr) {
				if (const auto cached = session->nodeLabelToken(labels.front()); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.label_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.composite_range_index_label_token_resolution", 1);
			const std::optional<uint64_t> resolved =
					system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, labels.front());
			if (session != nullptr) {
				session->putNodeLabelToken(labels.front(), resolved);
			}
			return resolved;
		};
		const auto resolvePropertyToken = [&](const std::string &name) {
			if (session != nullptr) {
				if (const auto cached = session->propertyToken(name); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.property_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.composite_range_index_property_token_resolution", 1);
			const std::optional<uint32_t> resolved = system_catalog_->findPropertyToken(snapshot, name);
			if (session != nullptr) {
				session->putPropertyToken(name, resolved);
			}
			return resolved;
		};
		const auto isBound = [](const PropertyValue &value) { return value.getType() != PropertyType::NULL_TYPE; };
		const auto isIndexable = [isBound](const PropertyValue &value) {
			if (!isBound(value)) return true;
			const PropertyType type = value.getType();
			return (type == PropertyType::BOOLEAN || type == PropertyType::INTEGER || type == PropertyType::DOUBLE ||
					type == PropertyType::STRING) &&
					(type != PropertyType::DOUBLE || std::isfinite(std::get<double>(value.getVariant())));
		};
		if (labels.size() != 1U || labels.front().empty() || request.equality_prefix.empty() || request.range.key.empty() ||
			(!isBound(request.range.min_value) && !isBound(request.range.max_value))) {
			return std::nullopt;
		}
		std::map<std::string, const PropertyValue *> prefix;
		for (const auto &[name, value]: request.equality_prefix) {
			if (name.empty() || name == request.range.key || !isIndexable(value) || !prefix.emplace(name, &value).second) {
				return std::nullopt;
			}
		}
		if (!isIndexable(request.range.min_value) || !isIndexable(request.range.max_value)) return std::nullopt;
		if (isBound(request.range.min_value) && isBound(request.range.max_value) &&
			(request.range.min_value > request.range.max_value ||
			 (request.range.min_value == request.range.max_value &&
			  (!request.range.min_inclusive || !request.range.max_inclusive)))) {
			return std::vector<LogicalGraphNode>{};
		}
		const auto labelToken = resolveLabelToken();
		if (!labelToken.has_value()) return std::vector<LogicalGraphNode>{};
		for (const auto &[name, value]: request.equality_prefix) {
			static_cast<void>(value);
			if (!resolvePropertyToken(name).has_value()) {
				return std::vector<LogicalGraphNode>{};
			}
		}
		if (!resolvePropertyToken(request.range.key).has_value()) {
			return std::vector<LogicalGraphNode>{};
		}
		if (options_.maximum_online_index_entries == 0U ||
			options_.maximum_online_index_entries == std::numeric_limits<size_t>::max()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable composite range Node lookup requires a finite Index-entry bound");
		}
		const std::vector<IndexDefinition> *cachedIndexes =
				session == nullptr ? nullptr : session->indexDefinitions(options_.maximum_index_definitions);
		std::vector<IndexDefinition> loadedIndexes;
		if (cachedIndexes == nullptr) {
			debug::PerfTrace::addValue("native_graph.read.composite_range_index_definition_read", 1);
			loadedIndexes = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) {
				session->putIndexDefinitions(loadedIndexes);
				cachedIndexes = session->indexDefinitions(options_.maximum_index_definitions);
			}
		}
		const std::vector<IndexDefinition> &definitions = cachedIndexes == nullptr ? loadedIndexes : *cachedIndexes;
		for (const IndexDefinition &definition: definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
				definition.kind != IndexDefinitionKind::IDK_COMPOSITE || definition.label_token_id != *labelToken ||
				definition.property_names.size() != prefix.size() + 1U || definition.property_names.back() != request.range.key) continue;
			std::vector<PropertyValue> prefixValues;
			prefixValues.reserve(definition.property_names.size());
			std::vector<uint32_t> propertyTokens;
			bool matches = true;
			for (size_t index = 0U; index + 1U < definition.property_names.size(); ++index) {
				const auto value = prefix.find(definition.property_names[index]);
				const auto token = resolvePropertyToken(definition.property_names[index]);
				if (value == prefix.end() || !token.has_value()) { matches = false; break; }
				prefixValues.push_back(*value->second);
				propertyTokens.push_back(*token);
			}
			const auto rangeToken = resolvePropertyToken(request.range.key);
			if (!matches || !rangeToken.has_value()) continue;
			propertyTokens.push_back(*rangeToken);
			const auto encodedPrefix = IndexKeyCodec::tryEncodeComposite(prefixValues);
			if (!encodedPrefix.has_value()) return std::nullopt;
			// Reuse one prefix workspace for both bounds instead of copying every
			// prefix PropertyValue (including strings) into two additional vectors.
			const auto encodeBound = [&](const PropertyValue &value, uint64_t entityId) {
				prefixValues.push_back(value);
				auto encoded = IndexKeyCodec::tryEncodeCompositeEntryKey(prefixValues, entityId);
				prefixValues.pop_back();
				return encoded;
			};
			std::optional<std::vector<std::byte>> lower;
				if (isBound(request.range.min_value)) {
					lower = encodeBound(request.range.min_value,
							request.range.min_inclusive ? 1U : std::numeric_limits<uint64_t>::max());
					if (!lower.has_value()) return std::nullopt;
				}
			if (!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable composite range Node lookup found an Index definition without a B+Tree root");
			}
			const BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			if (candidateOutput != nullptr && visitor != nullptr) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable composite range Node lookup cannot collect and stream candidates simultaneously");
			}
			if (candidateOutput != nullptr) {
				candidateOutput->predicates_verified = true;
				for (const std::string &name: definition.property_names) {
					candidateOutput->verified_property_columns.push_back({name, {}});
				}
			}
				const std::optional<std::span<const std::byte>> start =
						lower.has_value() ? std::optional(std::span<const std::byte>(*lower))
										: std::optional(std::span<const std::byte>(*encodedPrefix));
				// The lower bound for an exclusive value uses the largest possible
				// Entity-ID suffix. Exclude that sentinel itself so an entry with the
				// maximum ID cannot leak into the exclusive range.
				const bool excludeLowerBound = isBound(request.range.min_value) && !request.range.min_inclusive;
				auto cursor = index.openRangeCursor(snapshot, start, options_.maximum_online_index_entries,
					BufferPoolReadHint::BPRH_SEQUENTIAL, excludeLowerBound);
				debug::PerfTrace::addValue("native_graph.read.index_candidate_root_descent", 1);
				std::optional<std::vector<std::byte>> upperExclusive;
				if (isBound(request.range.max_value)) {
					upperExclusive = encodeBound(request.range.max_value,
							request.range.max_inclusive ? std::numeric_limits<uint64_t>::max() : 1U);
					if (!upperExclusive.has_value()) return std::nullopt;
				}
			std::vector<PropertyValue>().swap(prefixValues);
			CandidateDedupMemoryResource dedupMemory(maximumCandidateBytes);
			std::pmr::unordered_set<uint64_t> seen(&dedupMemory);
			std::vector<LogicalGraphNode> result;
			std::vector<uint64_t> resultNodeIds;
			std::vector<BTreePageEntry> pending;
			const size_t batchCapacity = candidateBatchCapacity(maximumNodes, maximumCandidateBytes);
			if (visitor != nullptr) {
			pending.reserve(batchCapacity);
			}
			size_t boundaryKeyBytes = 0U;
			const std::function<void(size_t)> admitBoundaryKey = [&](size_t bytes) {
				size_t retained = dedupMemory.retainedBytes();
				const auto add = [&](size_t count, size_t width) {
					if (count > (maximumCandidateBytes - retained) / width) {
						throw FormatException(FormatError::FERR_INVALID_STATE, "range cursor boundary exceeded its byte bound");
					}
					retained += count * width;
				};
				add(pending.capacity(), sizeof(BTreePageEntry));
				for (const auto &entry: pending) add(entry.key.capacity(), 1U);
				add(bytes, 1U);
				boundaryKeyBytes = bytes;
			};
			dedupMemory.setAllocationAdmission([&](size_t bytes) {
				const size_t retainedBoundary = boundaryKeyBytes;
				if (bytes > maximumCandidateBytes - retainedBoundary) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "range dedup allocation exceeded its shared byte bound");
				}
				admitBoundaryKey(retainedBoundary + bytes);
				boundaryKeyBytes = retainedBoundary;
			});
			size_t totalCandidates = 0U;
			bool rangeExhausted = false;
			struct CompositeSourceBytePressure {};
			const auto emitOnce = [&](const std::span<const BTreePageEntry> entries) -> bool {
				if (entries.empty()) return true;
				size_t candidateStagingBytes = dedupMemory.retainedBytes();
				if (boundaryKeyBytes > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
				candidateStagingBytes += boundaryKeyBytes;
				// The whole pending batch stays alive during recursive splits.
				// Charge its slots and owned keys, not only the emitted subspan.
				if (pending.capacity() > (maximumCandidateBytes - candidateStagingBytes) / sizeof(BTreePageEntry))
					throw CompositeSourceBytePressure{};
				candidateStagingBytes += pending.capacity() * sizeof(BTreePageEntry);
				for (const auto &entry: pending) {
					if (entry.key.capacity() > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
					candidateStagingBytes += entry.key.capacity();
				}
				size_t labelBufferBytes = 0U;
				const auto checkSlots = [&](size_t count, size_t width) {
					if (count > (maximumCandidateBytes - candidateStagingBytes - labelBufferBytes) / width)
						throw CompositeSourceBytePressure{};
				};
				checkSlots(entries.size(), sizeof(uint64_t));
				std::vector<uint64_t> nodeIds;
				nodeIds.reserve(entries.size());
				checkSlots(nodeIds.capacity(), sizeof(uint64_t));
				candidateStagingBytes += nodeIds.capacity() * sizeof(uint64_t);
				for (const BTreePageEntry &candidate: entries) {
					if (candidate.key.size() <= encodedPrefix->size() + sizeof(uint64_t) ||
							!std::equal(encodedPrefix->begin(), encodedPrefix->end(), candidate.key.begin())) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup found a malformed Index key");
					}
					const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate.key);
					if (candidate.value_or_child != nodeId) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup found a noncanonical Index entry");
					}
					nodeIds.push_back(nodeId);
				}
				checkSlots(nodeIds.size(), sizeof(size_t));
				std::vector<size_t> propertyOrder(nodeIds.size());
				checkSlots(propertyOrder.capacity(), sizeof(size_t));
				candidateStagingBytes += propertyOrder.capacity() * sizeof(size_t);
				std::iota(propertyOrder.begin(), propertyOrder.end(), 0U);
				std::ranges::sort(propertyOrder, [&nodeIds](size_t left, size_t right) {
					return nodeIds[left] < nodeIds[right];
				});
				std::vector<uint64_t> sortedNodeIds;
				checkSlots(propertyOrder.size(), sizeof(uint64_t));
				sortedNodeIds.reserve(propertyOrder.size());
				checkSlots(sortedNodeIds.capacity(), sizeof(uint64_t));
				candidateStagingBytes += sortedNodeIds.capacity() * sizeof(uint64_t);
				for (const size_t index: propertyOrder) sortedNodeIds.push_back(nodeIds[index]);
				const std::array<uint64_t, 1U> labelTokens{definition.label_token_id};
				const SelectedPropertyReadAdmission admitLabelBuffers = [&](size_t bytes) {
					if (bytes > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
					labelBufferBytes = bytes;
				};
				const auto labelMatches = native_graph_reads_->readNodeLabelMatchesSorted(
						snapshot, sortedNodeIds, labelTokens, session, &admitLabelBuffers);
				if (labelMatches.size() != sortedNodeIds.size() ||
					std::ranges::any_of(labelMatches, [](bool matches) { return !matches; })) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable composite range Node lookup found an invalid indexed label");
				}

				std::vector<std::pair<uint32_t, size_t>> tokenOrder;
				checkSlots(propertyTokens.size(), sizeof(std::pair<uint32_t, size_t>));
				tokenOrder.reserve(propertyTokens.size());
				checkSlots(tokenOrder.capacity(), sizeof(std::pair<uint32_t, size_t>));
				candidateStagingBytes += tokenOrder.capacity() * sizeof(std::pair<uint32_t, size_t>);
				if (labelBufferBytes > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
				for (size_t index = 0U; index < propertyTokens.size(); ++index) {
					tokenOrder.emplace_back(propertyTokens[index], index);
				}
				std::ranges::sort(tokenOrder, {}, &std::pair<uint32_t, size_t>::first);
				for (size_t index = 1U; index < tokenOrder.size(); ++index) {
					if (tokenOrder[index - 1U].first == tokenOrder[index].first) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup resolved duplicate Property tokens");
					}
				}
				std::vector<uint32_t> sortedPropertyTokens;
				checkSlots(tokenOrder.size(), sizeof(uint32_t));
				sortedPropertyTokens.reserve(tokenOrder.size());
				checkSlots(sortedPropertyTokens.capacity(), sizeof(uint32_t));
				candidateStagingBytes += sortedPropertyTokens.capacity() * sizeof(uint32_t);
				if (labelBufferBytes > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
				for (const auto &[token, originalColumn]: tokenOrder) {
					static_cast<void>(originalColumn);
					sortedPropertyTokens.push_back(token);
				}
				size_t selectedSourceBytes = candidateStagingBytes;
				size_t largeValueBufferBytes = labelBufferBytes;
				const SelectedPropertyReadAdmission admission = [&](size_t bytes) {
					if (bytes > maximumCandidateBytes - selectedSourceBytes ||
						largeValueBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes)
						throw CompositeSourceBytePressure{};
					selectedSourceBytes += bytes;
					traceNativeCandidateMetric("native_graph.read.composite_range_candidate_source_bytes", bytes);
				};
				const SelectedPropertyReadAdmission admitLargeValueBuffers = [&](size_t bytes) {
					if (bytes > maximumCandidateBytes - selectedSourceBytes ||
						labelBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes) throw CompositeSourceBytePressure{};
					largeValueBufferBytes = labelBufferBytes + bytes;
				};
				auto columnsAndCounts = native_graph_reads_->readNodePropertyColumnsAndCountsSorted(
						snapshot, sortedNodeIds, sortedPropertyTokens, session, &admission, &admitLargeValueBuffers);
				auto &sortedPropertiesByToken = columnsAndCounts.first;
				const auto &propertyCounts = columnsAndCounts.second;
				if (labelMatches.size() != sortedNodeIds.size() || propertyCounts.size() != sortedNodeIds.size()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable composite range Node lookup returned an invalid candidate validation batch");
				}
				traceNativeCandidateMetric("native_graph.read.index_candidate_liveness_probes", sortedNodeIds.size());
				for (size_t index = 0U; index < sortedNodeIds.size(); ++index) {
					if (!labelMatches[index] || !propertyCounts[index].has_value() ||
							*propertyCounts[index] > maximumPropertyEntries) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup found an invalid indexed Node");
					}
				}
				if (sortedPropertiesByToken.size() != propertyTokens.size()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable composite range Node lookup returned an invalid Property column batch");
				}
				for (const auto &column: sortedPropertiesByToken) {
					if (column.size() != propertyOrder.size()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup returned an invalid Property batch");
					}
				}
				// Reuse both permutations to scatter rows and columns without allocating
				// another matrix of optional PropertyValue slots.
				auto &propertiesByToken = sortedPropertiesByToken;
				for (size_t index = 0U; index < propertyOrder.size(); ++index) {
					while (propertyOrder[index] != index) {
						const size_t destination = propertyOrder[index];
						for (auto &column: propertiesByToken) std::swap(column[index], column[destination]);
						std::swap(propertyOrder[index], propertyOrder[destination]);
					}
				}
				for (size_t index = 0U; index < tokenOrder.size(); ++index) {
					while (tokenOrder[index].second != index) {
						const size_t destination = tokenOrder[index].second;
						std::swap(propertiesByToken[index], propertiesByToken[destination]);
						std::swap(tokenOrder[index], tokenOrder[destination]);
					}
				}
				size_t outputBufferBytes = 0U;
				const SelectedPropertyReadAdmission admitOutput = [&](size_t bytes) {
					if (bytes > maximumCandidateBytes - selectedSourceBytes ||
						largeValueBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes) throw CompositeSourceBytePressure{};
					outputBufferBytes = bytes;
				};
				IndexCandidateBatch streamBatch;
				if (visitor != nullptr) {
					streamBatch.snapshot_generation = snapshot.superblock().generation.value();
					streamBatch.predicates_verified = true;
					streamBatch.label_scope_verified = true;
					reserveCandidateOutput(streamBatch, entries.size(), definition.property_names, admitOutput);
				}
				// One row workspace is reused; its slots coexist with reader/output arrays.
				std::vector<PropertyValue> actual;
				const size_t outputSlots = outputBufferBytes;
				if (propertyTokens.size() > (maximumCandidateBytes - selectedSourceBytes - largeValueBufferBytes - outputSlots) / sizeof(PropertyValue))
					throw CompositeSourceBytePressure{};
				admitOutput(outputSlots + propertyTokens.size() * sizeof(PropertyValue));
				actual.reserve(propertyTokens.size());
				admitOutput(outputSlots + actual.capacity() * sizeof(PropertyValue));
				for (size_t index = 0U; index < entries.size(); ++index) {
					const uint64_t nodeId = nodeIds[index];
					actual.clear();
					for (auto &properties: propertiesByToken) {
						if (!properties[index].has_value()) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable composite range Node lookup found a stale Index entry");
						}
						actual.push_back(std::move(*properties[index]));
					}
					const auto expected = IndexKeyCodec::tryEncodeCompositeEntryKey(actual, nodeId);
					if (!expected.has_value() || entries[index].key != *expected) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup found a noncanonical Index entry");
					}
					const PropertyValue &value = actual.back();
					if (isBound(request.range.max_value) &&
							(request.range.max_inclusive ? value > request.range.max_value : value >= request.range.max_value)) {
						rangeExhausted = true;
						continue;
					}
					const bool belowLower = isBound(request.range.min_value) &&
							(request.range.min_inclusive ? value < request.range.min_value : value <= request.range.min_value);
					if (belowLower) continue;
					if (candidateOutput != nullptr) {
						for (size_t propertyIndex = 0U; propertyIndex < definition.property_names.size(); ++propertyIndex) {
							candidateOutput->verified_property_columns[propertyIndex].values.push_back(std::move(actual[propertyIndex]));
						}
						candidateOutput->candidates.push_back({nodeId, false, 0U, 0U, 0U});
						traceNativeCandidateMetric("native_graph.read.index_candidate_verified_rows", 1U);
					} else if (visitor != nullptr) {
						streamBatch.candidates.push_back({nodeId, false, 0U, 0U, 0U});
						for (size_t propertyIndex = 0U; propertyIndex < actual.size(); ++propertyIndex) {
							streamBatch.verified_property_columns[propertyIndex].values.push_back(std::move(actual[propertyIndex]));
						}
					} else {
						resultNodeIds.push_back(nodeId);
					}
				}
				if (visitor == nullptr) return true;
				// Reuse the already admitted row permutation for output ordering.
				propertyOrder.resize(streamBatch.candidates.size());
				auto &order = propertyOrder;
				std::iota(order.begin(), order.end(), 0U);
				std::ranges::sort(order, [&streamBatch](size_t left, size_t right) {
					return streamBatch.candidates[left].entity_id < streamBatch.candidates[right].entity_id;
				});
				reorderCandidateRowsInPlace(streamBatch, order);
				traceNativeCandidateMetric("native_graph.read.index_candidate_batches", streamBatch.candidates.empty() ? 0U : 1U);
				traceNativeCandidateMetric("native_graph.read.index_candidate_emitted_rows", streamBatch.candidates.size());
				enforceCandidateBatchBytes(streamBatch, maximumCandidateBytes);
				return streamBatch.candidates.empty() || (*visitor)(streamBatch);
			};
			std::function<bool(std::span<const BTreePageEntry>)> emit;
			emit = [&](std::span<const BTreePageEntry> entries) -> bool {
				try {
					return emitOnce(entries);
				} catch (const CompositeSourceBytePressure &) {
					if (entries.size() <= 1U) {
						throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable native index candidate batch exceeded its byte bound");
					}
				}
				traceNativeCandidateMetric("native_graph.read.composite_range_candidate_source_splits", 1U);
				const size_t left = entries.size() / 2U;
				return emit(entries.first(left)) && emit(entries.subspan(left));
			};

				for (;;) {
					if (rangeExhausted) break;
					auto candidate = cursor.next(admitBoundaryKey);
					if (!candidate.has_value() || candidate->key.size() <= encodedPrefix->size() + sizeof(uint64_t) ||
							!std::equal(encodedPrefix->begin(), encodedPrefix->end(), candidate->key.begin())) break;
					if (upperExclusive.has_value()) {
						const bool outsideUpper = request.range.max_inclusive
								? std::lexicographical_compare(upperExclusive->begin(), upperExclusive->end(),
										candidate->key.begin(), candidate->key.end())
								: !std::lexicographical_compare(candidate->key.begin(), candidate->key.end(),
										upperExclusive->begin(), upperExclusive->end());
						if (outsideUpper) break;
					}
					const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate->key);
				if (candidate->value_or_child != nodeId || !seen.insert(nodeId).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable composite range Node lookup found a noncanonical Index entry");
				}
				if (++totalCandidates > maximumNodes) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable composite range Node lookup exceeded its caller-supplied result bound");
				}
				pending.push_back(std::move(*candidate));
				if (pending.size() == batchCapacity) {
					if (!emit(std::span<const BTreePageEntry>(pending))) {
						if (visitorCompleted != nullptr) *visitorCompleted = false;
						return std::vector<LogicalGraphNode>{};
					}
					pending.clear();
				}
			}
			if (!pending.empty()) {
				if (!emit(std::span<const BTreePageEntry>(pending)) && visitorCompleted != nullptr) *visitorCompleted = false;
			}
			if (visitor != nullptr) return std::vector<LogicalGraphNode>{};
			if (visitor == nullptr && candidateOutput == nullptr && !resultNodeIds.empty()) {
				std::ranges::sort(resultNodeIds);
				result = native_logical_graph_reads_->readNodesShapedSorted(
						snapshot, resultNodeIds, {.include_labels = true, .all_properties = true}, session);
			}
			std::ranges::sort(result, {}, &LogicalGraphNode::node_id);
			if (candidateOutput != nullptr) {
				std::vector<size_t> order(candidateOutput->candidates.size());
				std::iota(order.begin(), order.end(), 0U);
				std::ranges::sort(order, [candidateOutput](size_t left, size_t right) {
					return candidateOutput->candidates[left].entity_id < candidateOutput->candidates[right].entity_id;
				});
				reorderCandidateRowsInPlace(*candidateOutput, order);
			}
			return result;
		}
		return std::nullopt;
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNodesByCompositePropertyRange(
			const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphCompositePropertyRange &request, size_t maximumNodes, size_t maximumPropertyEntries) const {
		requireOwnedSnapshot(snapshot);
		return findNativeNodesByCompositePropertyRange(snapshot.pin_, labels, request, maximumNodes, maximumPropertyEntries);
	}

	std::optional<IndexCandidateBatch> RelocatableStorageBackend::findNativeNodeCandidatesByCompositePropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphCompositePropertyRange &request, size_t maximumNodes,
			size_t maximumPropertyEntries, NativeReadSession *session) const {
		IndexCandidateBatch result;
		const auto complete = findNativeNodesByCompositePropertyRange(snapshot, labels, request, maximumNodes,
														 maximumPropertyEntries, session, &result);
		if (!complete.has_value()) {
			return std::nullopt;
		}
		if (!result.predicates_verified || result.candidates.size() > maximumNodes) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
										"relocatable native composite range Node lookup returned an invalid candidate batch");
		}
		for (const auto &column: result.verified_property_columns) {
			if (column.values.size() != result.candidates.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
										"relocatable native composite range Node lookup returned an invalid Property column");
			}
		}
		// The composite source already orders the complete batch and its columns.
		if (!std::ranges::is_sorted(result.candidates, {}, &EntityRef::entity_id)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable native composite range Node lookup returned an unsorted candidate batch");
		}
		return result;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeNodeCandidatesByCompositePropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphCompositePropertyRange &request, const size_t maximumNodes,
			const size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor,
			NativeReadSession *session, size_t maximumCandidateBytes) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable native composite range Node candidate stream requires a visitor");
		}
		bool completed = true;
		IndexCandidateBatchVisitor callback = visitor;
		const auto result = findNativeNodesByCompositePropertyRange(snapshot, labels, request, maximumNodes,
				maximumPropertyEntries, session, nullptr, &callback, &completed, maximumCandidateBytes);
		if (!result.has_value()) {
			return std::nullopt;
		}
		return completed;
	}

	void RelocatableStorageBackend::visitNodes(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
											   const std::function<void(const LogicalGraphNode &)> &visitor) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER, "relocatable Node scan requires a visitor");
		}
		static_cast<void>(visitNodesUntil(snapshot, maximumRecords, [&visitor](const LogicalGraphNode &node) {
			visitor(node);
			return true;
		}));
	}

	void RelocatableStorageBackend::visitEdges(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
											   const std::function<void(const LogicalGraphEdge &)> &visitor) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER, "relocatable Edge scan requires a visitor");
		}
		static_cast<void>(visitEdgesUntil(snapshot, maximumRecords, [&visitor](const LogicalGraphEdge &edge) {
			visitor(edge);
			return true;
		}));
	}

	bool
	RelocatableStorageBackend::visitNodesUntil(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
											   const std::function<bool(const LogicalGraphNode &)> &visitor) const {
		requireOwnedSnapshot(snapshot);
		if (maximumRecords == 0U || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Node scan requires a positive bound and visitor");
		}
		return native_logical_graph_reads_->visitNodesUntil(snapshot.pin_, maximumRecords, visitor);
	}

	bool
	RelocatableStorageBackend::visitEdgesUntil(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
											   const std::function<bool(const LogicalGraphEdge &)> &visitor) const {
		requireOwnedSnapshot(snapshot);
		if (maximumRecords == 0U || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Edge scan requires a positive bound and visitor");
		}
		return native_logical_graph_reads_->visitEdgesUntil(snapshot.pin_, maximumRecords, visitor);
	}

	LogicalGraphNodePage RelocatableStorageBackend::scanNodesAfter(const RelocatableReadSnapshot &snapshot,
																   uint64_t afterNodeId, size_t maximumRecords,
																   size_t maximumMetadataPages) const {
		requireOwnedSnapshot(snapshot);
		if (maximumRecords == 0U || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable resumable Node scan requires positive bounds");
		}
		return native_logical_graph_reads_->scanNodesAfter(snapshot.pin_, afterNodeId, maximumRecords,
														   maximumMetadataPages);
	}

	LogicalGraphAdjacencyPage RelocatableStorageBackend::scanAdjacentEdges(
			const RelocatableReadSnapshot &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const {
		requireOwnedSnapshot(snapshot);
		return native_logical_graph_reads_->scanAdjacentEdges(snapshot.pin_, nodeId, direction, std::move(continuation),
															  maximumRecords);
	}

	void
	RelocatableStorageBackend::visitAdjacentEdges(const RelocatableReadSnapshot &snapshot, uint64_t nodeId,
												  LogicalGraphReadDirection direction, size_t maximumEdges,
												  const std::function<void(const LogicalGraphEdge &)> &visitor) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable adjacency traversal requires a visitor");
		}
		static_cast<void>(visitAdjacentEdgesUntil(snapshot, nodeId, direction, maximumEdges,
												  [&visitor](const LogicalGraphEdge &edge) {
													  visitor(edge);
													  return true;
												  }));
	}

	bool RelocatableStorageBackend::visitAdjacentEdgesUntil(
			const RelocatableReadSnapshot &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			size_t maximumEdges, const std::function<bool(const LogicalGraphEdge &)> &visitor) const {
		requireOwnedSnapshot(snapshot);
		if (maximumEdges == 0U || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable adjacency traversal requires a positive bound and visitor");
		}
		return native_logical_graph_reads_->visitAdjacentEdgesUntil(snapshot.pin_, nodeId, direction, maximumEdges,
																	visitor);
	}

	std::optional<uint32_t> RelocatableStorageBackend::findPropertyKeyToken(const RelocatableReadSnapshot &snapshot,
																			std::string_view key) const {
		requireOwnedSnapshot(snapshot);
		return system_catalog_->findPropertyToken(snapshot.pin_, key);
	}

	std::optional<std::string> RelocatableStorageBackend::findPropertyKey(const RelocatableReadSnapshot &snapshot,
																		  uint32_t token) const {
		requireOwnedSnapshot(snapshot);
		return system_catalog_->findPropertyName(snapshot.pin_, token);
	}

	std::optional<TokenDefinition> RelocatableStorageBackend::findSchemaToken(const RelocatableReadSnapshot &snapshot,
																			  uint64_t tokenId) const {
		requireOwnedSnapshot(snapshot);
		const std::optional<SystemCatalogSchemaToken> label =
				system_catalog_->findSchemaToken(snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_LABEL, tokenId);
		const std::optional<SystemCatalogSchemaToken> relationshipType = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, tokenId);
		if (label.has_value() && relationshipType.has_value()) { // SystemCatalog allocates both schema domains from one monotonic token sequence, so a valid snapshot cannot bind one ID twice.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable schema Token ID is ambiguous across native schema domains");
		}
		const std::optional<SystemCatalogSchemaToken> token = label.has_value() ? label : relationshipType;
		if (!token.has_value()) {
			return std::nullopt;
		}
		return TokenDefinition{token->token_id, token->name};
	}

	std::optional<uint64_t> RelocatableStorageBackend::findSchemaTokenId(const RelocatableReadSnapshot &snapshot,
																					 std::string_view name) const {
		requireOwnedSnapshot(snapshot);
		const std::optional<uint64_t> label =
				system_catalog_->findSchemaToken(snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
		const std::optional<uint64_t> relationshipType = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, name);
		if (label.has_value() && relationshipType.has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable schema Token name is ambiguous across native schema domains");
		}
		return label.has_value() ? label : relationshipType;
	}

	std::optional<uint64_t>
	RelocatableStorageBackend::findRelationshipTypeTokenId(const RelocatableReadSnapshot &snapshot,
																								std::string_view name) const {
		requireOwnedSnapshot(snapshot);
		if (name.empty()) {
			return std::nullopt;
		}
		debug::PerfTrace::addValue("native_graph.read.relationship_type_token_resolution", 1);
		return system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, name);
	}

	std::vector<IndexDefinition>
	RelocatableStorageBackend::readIndexDefinitions(const RelocatableReadSnapshot &snapshot,
													size_t maximumDefinitions) const {
		requireOwnedSnapshot(snapshot);
		return readAllIndexDefinitions(snapshot.pin_, maximumDefinitions);
	}

	LogicalGraphVectorSearchResult
	RelocatableStorageBackend::searchLogicalVector(const RelocatableReadSnapshot &snapshot,
												   const LogicalGraphVectorSearchRequest &request) const {
		requireOwnedSnapshot(snapshot);
		if (request.index_name.empty() || request.query_vector.empty() || request.top_k == 0U ||
			request.maximum_mapping_entries == 0U || request.maximum_raw_vector_reads == 0U ||
			request.maximum_mapping_entries == std::numeric_limits<size_t>::max() ||
			request.maximum_raw_vector_reads == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument(
					"relocatable logical Vector search requires a name, query, result count, and finite bounds");
		}
		if (!std::ranges::all_of(request.query_vector, [](float value) { return std::isfinite(value); })) {
			throw std::invalid_argument("relocatable logical Vector search query contains a non-finite coordinate");
		}
		if (request.mode != LogicalGraphVectorSearchMode::LGVSM_AUTO &&
			request.mode != LogicalGraphVectorSearchMode::LGVSM_EXACT) {
			throw std::invalid_argument("relocatable logical Vector search received an unknown search mode");
		}

		const std::vector<IndexDefinition> definitions =
				readAllIndexDefinitions(snapshot.pin_, options_.maximum_index_definitions);
		const auto definition = std::ranges::find_if(
				definitions, [&](const IndexDefinition &candidate) { return candidate.name == request.index_name; });
		if (definition == definitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR ||
			definition->entity_kind != IndexDefinitionEntityKind::IDEK_NODE) {
			throw std::runtime_error("Vector index not found: " + request.index_name);
		}
		if (request.query_vector.size() != definition->vector_dimensions) {
			throw std::invalid_argument("relocatable logical Vector search query dimension does not match its Index");
		}
		if (definition->vector_metric == "cosine" &&
			!std::ranges::any_of(request.query_vector, [](float value) { return value != 0.0F; })) {
			throw std::invalid_argument("relocatable cosine Vector search query must have a nonzero L2 norm");
		}
		// An index may validly precede its first matching Property. In that state no
		// physical mapping or Manifest exists yet, so the exact answer is empty.
		if (!metadata_->findVectorManifestOptional(snapshot.pin_, definition->index_id).has_value()) {
			return {};
		}
		const VectorManifest manifest = vector_manifests_->read(snapshot.pin_, definition->index_id).manifest();
		VectorAdjacencyPageStore adjacencies(data_file_->io(), *metadata_,
											 isReadOnly() ? SharedPagePoolOpenMode::SPPOM_READ_ONLY
														  : SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
		return RelocatableVectorSearch::search(snapshot.pin_, *definition, manifest, request, *btree_pages_,
											   *vector_pages_, adjacencies, *vector_codebooks_, *metadata_);
	}

	std::optional<LogicalGraphVectorIndexDescriptor>
	RelocatableStorageBackend::describeLogicalVectorIndex(const RelocatableReadSnapshot &snapshot,
														  const LogicalGraphVectorSearchRequest &request) const {
		requireOwnedSnapshot(snapshot);
		if (request.index_name.empty() || request.query_vector.empty() || request.top_k == 0U ||
			request.maximum_mapping_entries == 0U || request.maximum_raw_vector_reads == 0U ||
			request.maximum_mapping_entries == std::numeric_limits<size_t>::max() ||
			request.maximum_raw_vector_reads == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument(
					"relocatable logical Vector search requires a name, query, result count, and finite bounds");
		}
		if (!std::ranges::all_of(request.query_vector, [](float value) { return std::isfinite(value); })) {
			throw std::invalid_argument("relocatable logical Vector search query contains a non-finite coordinate");
		}
		if (request.mode != LogicalGraphVectorSearchMode::LGVSM_AUTO &&
			request.mode != LogicalGraphVectorSearchMode::LGVSM_EXACT) {
			throw std::invalid_argument("relocatable logical Vector search received an unknown search mode");
		}
		const std::vector<IndexDefinition> definitions =
				readAllIndexDefinitions(snapshot.pin_, options_.maximum_index_definitions);
		const auto definition = std::ranges::find_if(
				definitions, [&](const IndexDefinition &candidate) { return candidate.name == request.index_name; });
		if (definition == definitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR ||
			definition->entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
			definition->property_names.size() != 1U || definition->label_token_id == 0U ||
			definition->vector_dimensions == 0U) {
			throw std::runtime_error("Vector index not found: " + request.index_name);
		}
		if (request.query_vector.size() != definition->vector_dimensions) {
			throw std::invalid_argument("relocatable logical Vector search query dimension does not match its Index");
		}
		if (definition->vector_metric == "cosine" &&
			!std::ranges::any_of(request.query_vector, [](float value) { return value != 0.0F; })) {
			throw std::invalid_argument("relocatable cosine Vector search query must have a nonzero L2 norm");
		}
		const std::optional<SystemCatalogSchemaToken> label = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_LABEL, definition->label_token_id);
		if (!label.has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical Vector search found an unknown canonical label Token");
		}
		return LogicalGraphVectorIndexDescriptor{definition->index_id, label->name, definition->property_names.front(),
												 definition->vector_dimensions, definition->vector_metric};
	}

	std::optional<LogicalGraphVectorSearchHit> RelocatableStorageBackend::scoreLogicalVectorExactOverlayCandidate(
			const RelocatableReadSnapshot &snapshot, const LogicalGraphVectorSearchRequest &request,
			const LogicalGraphVectorIndexDescriptor &index, const LogicalGraphNode &node) const {
		requireOwnedSnapshot(snapshot);
		// The descriptor is obtained once from this immutable snapshot before the
		// bounded scan starts. Re-reading the Catalog for every candidate would
		// turn the exact fallback into O(N * catalog-size) metadata work.
		if (index.index_id == 0U || index.label.empty() || index.property_key.empty() || index.dimensions == 0U ||
			request.query_vector.size() != index.dimensions ||
			(index.metric != "l2" && index.metric != "ip" && index.metric != "cosine")) {
			throw std::logic_error("logical Vector exact overlay candidate received an invalid Index descriptor");
		}
		if (!std::ranges::binary_search(node.labels, index.label)) {
			return std::nullopt;
		}
		const auto property = std::ranges::find_if(node.properties, [&](const LogicalGraphPropertyEntry &entry) {
			return entry.key == index.property_key;
		});
		if (property == node.properties.end()) {
			return std::nullopt;
		}
		const std::vector<std::byte> raw = VectorSemanticCodec::encodeBFloat16(property->value, index.dimensions);
		double score = 0.0;
		double querySquaredNorm = 0.0;
		double candidateSquaredNorm = 0.0;
		for (uint32_t dimension = 0U; dimension < index.dimensions; ++dimension) {
			common::BFloat16 encoded;
			encoded.data = LittleEndianCodec::readU16(raw, static_cast<size_t>(dimension) * sizeof(uint16_t));
			const float candidate = encoded.toFloat();
			if (!std::isfinite(candidate)) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "logical Vector exact overlay candidate decoded a non-finite coordinate");
			}
			if (index.metric == "l2") {
				const double difference = static_cast<double>(request.query_vector[dimension]) - candidate;
				score += difference * difference;
			} else {
				const double query = static_cast<double>(request.query_vector[dimension]);
				score += query * candidate;
				querySquaredNorm += query * query;
				candidateSquaredNorm += static_cast<double>(candidate) * candidate;
			}
		}
		if (index.metric == "ip") {
			score = -score;
		} else if (index.metric == "cosine") {
			if (candidateSquaredNorm == 0.0) {
				throw std::invalid_argument("logical Vector exact overlay candidate has zero cosine norm");
			}
			score = 1.0 - std::clamp(score / std::sqrt(querySquaredNorm * candidateSquaredNorm), -1.0, 1.0);
		} else if (index.metric != "l2") {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "logical Vector exact overlay candidate has an unsupported metric");
		}
		if (!std::isfinite(score) || score > static_cast<double>(std::numeric_limits<float>::max()) ||
			score < -static_cast<double>(std::numeric_limits<float>::max())) {
			throw std::overflow_error("logical Vector exact overlay candidate produced a non-finite distance");
		}
		return LogicalGraphVectorSearchHit{node.node_id, static_cast<float>(score)};
	}

	std::unique_ptr<PreparedLogicalGraphVectorTraining>
	RelocatableStorageBackend::prepareLogicalVectorTraining(const LogicalGraphVectorTrainingRequest &request) {
		requireOpen();
		std::unique_lock<std::mutex> publicationLock(native_publication_mutex_);
		checkVectorTrainingCancellation(request);
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot train a logical Vector Index");
		}
		requireNoIndeterminateWrite();
		if (request.index_name.empty() || request.maximum_records == 0U || request.maximum_pages == 0U ||
			request.maximum_records == std::numeric_limits<size_t>::max() ||
			request.maximum_pages == std::numeric_limits<size_t>::max() ||
			options_.maximum_vector_training_records == 0U || options_.maximum_vector_training_pages == 0U ||
			options_.maximum_vector_training_samples == 0U || options_.vector_training_iterations == 0U) {
			throw std::invalid_argument("relocatable logical Vector training requires a nonempty index name and finite "
										"positive resource bounds");
		}
		const size_t maximumRecords = std::min(request.maximum_records, options_.maximum_vector_training_records);
		const size_t maximumPages = std::min(request.maximum_pages, options_.maximum_vector_training_pages);
		if (maximumRecords == 0U || maximumPages == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument("relocatable logical Vector training has an invalid effective resource bound");
		}

		// Keep the writer fence until commit so the status inspected by the result
		// pipeline cannot diverge from the generation ultimately published.
		MetadataSnapshotPin sourceSnapshot = metadata_->openSnapshot();
		const std::vector<IndexDefinition> definitions =
				readAllIndexDefinitions(sourceSnapshot, options_.maximum_index_definitions);
		const auto definition = std::ranges::find_if(
				definitions, [&](const IndexDefinition &candidate) { return candidate.name == request.index_name; });
		if (definition == definitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR ||
			definition->entity_kind != IndexDefinitionEntityKind::IDEK_NODE) {
			throw std::runtime_error("Vector index not found: " + request.index_name);
		}
		if (definition->vector_dimensions == 0U || definition->vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS) {
			failVectorTraining(FormatError::FERR_INVALID_HEADER,
							   "relocatable logical Vector training requires one valid Vector Index definition");
		}
		bool willTrain = false;
		if (metadata_->findVectorManifestOptional(sourceSnapshot, definition->index_id).has_value()) {
			const std::vector<BTreePageEntry> firstRecord =
					BTreeIndex(definition->index_id, *btree_pages_, *metadata_)
							.scan(sourceSnapshot, std::nullopt, 1U, BufferPoolReadHint::BPRH_SEQUENTIAL,
								  request.cancellation);
			willTrain = !firstRecord.empty();
		}
		sourceSnapshot.release();

		const LogicalGraphVectorTrainingPreview preview{willTrain};
		return std::make_unique<PreparedRelocatableVectorTraining>(
				std::move(publicationLock), preview, [this, request, preview] {
					const LogicalGraphVectorTrainingResult result = trainLogicalVectorUnderPublicationLock(request);
					if (result.trained != preview.will_train) {
						throw std::logic_error("relocatable Vector training preview diverged from its fenced source");
					}
					return result;
				});
	}

	LogicalGraphVectorTrainingResult RelocatableStorageBackend::trainLogicalVectorUnderPublicationLock(
			const LogicalGraphVectorTrainingRequest &request) {
		requireOpen();
		checkVectorTrainingCancellation(request);
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot train a logical Vector Index");
		}
		requireNoIndeterminateWrite();
		if (request.index_name.empty() || request.maximum_records == 0U || request.maximum_pages == 0U ||
			request.maximum_records == std::numeric_limits<size_t>::max() ||
			request.maximum_pages == std::numeric_limits<size_t>::max() ||
			options_.maximum_vector_training_records == 0U || options_.maximum_vector_training_pages == 0U ||
			options_.maximum_vector_training_samples == 0U || options_.vector_training_iterations == 0U) {
			throw std::invalid_argument("relocatable logical Vector training requires a nonempty index name and finite "
										"positive resource bounds");
		}

		const size_t maximumRecords = std::min(request.maximum_records, options_.maximum_vector_training_records);
		const size_t maximumPages = std::min(request.maximum_pages, options_.maximum_vector_training_pages);
		if (maximumRecords == 0U || maximumPages == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument("relocatable logical Vector training has an invalid effective resource bound");
		}

		LogicalGraphVectorTrainingResult result;
		{
			MetadataSnapshotPin sourceSnapshot = metadata_->openSnapshot();
			checkVectorTrainingCancellation(request);
			const std::vector<IndexDefinition> definitions =
					readAllIndexDefinitions(sourceSnapshot, options_.maximum_index_definitions);
			const auto definition = std::ranges::find_if(definitions, [&](const IndexDefinition &candidate) {
				return candidate.name == request.index_name;
			});
			if (definition == definitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR ||
				definition->entity_kind != IndexDefinitionEntityKind::IDEK_NODE) {
				throw std::runtime_error("Vector index not found: " + request.index_name);
			}
			if (definition->vector_dimensions == 0U ||
				definition->vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS) {
				failVectorTraining(FormatError::FERR_INVALID_HEADER,
								   "relocatable logical Vector training requires one valid Vector Index definition");
			}

			// A schema may exist before its first matching Property is inserted. There
			// is no physical semantic mapping to train in that valid empty state.
			if (!metadata_->findVectorManifestOptional(sourceSnapshot, definition->index_id).has_value()) {
				sourceSnapshot.release();
				return result;
			}
			const VectorManifest manifest = vector_manifests_->read(sourceSnapshot, definition->index_id).manifest();
			const std::optional<IndexRootLocation> forwardRoot =
					metadata_->findIndexRootOptional(sourceSnapshot, definition->index_id);
			const std::optional<IndexRootLocation> reverseRoot =
					metadata_->findIndexRootOptional(sourceSnapshot, manifest.vector_node_mapping_index_id);
			const uint8_t invalidSemanticManifest =
					static_cast<uint8_t>(manifest.index_id != definition->index_id) |
					static_cast<uint8_t>(manifest.dimensions != definition->vector_dimensions) |
					static_cast<uint8_t>(manifest.metric != VectorSemanticCodec::metricFor(*definition)) |
					static_cast<uint8_t>(!manifest.node_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(!manifest.node_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(!VectorNodeMapping::isMappingIndexId(manifest.vector_node_mapping_index_id)) |
					static_cast<uint8_t>(!manifest.vector_node_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(!manifest.vector_node_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(!forwardRoot.has_value()) | static_cast<uint8_t>(!reverseRoot.has_value()) |
					static_cast<uint8_t>(forwardRoot.has_value() &&
										 (forwardRoot->root_page_id != manifest.node_mapping_root_page_id ||
										  forwardRoot->root_generation != manifest.node_mapping_root_generation)) |
					static_cast<uint8_t>(
							reverseRoot.has_value() &&
							(reverseRoot->root_page_id != manifest.vector_node_mapping_root_page_id ||
							 reverseRoot->root_generation != manifest.vector_node_mapping_root_generation));
			if (invalidSemanticManifest != 0U) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found an invalid semantic Manifest generation");
			}
			if (hasAnyPqIdentity(manifest) && !hasCompletePqIdentity(manifest)) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found a partial PQ Manifest generation");
			}
			if (hasCompletePqIdentity(manifest)) {
				const std::optional<IndexRootLocation> pqRoot =
						metadata_->findIndexRootOptional(sourceSnapshot, manifest.pq_mapping_index_id);
				if (!pqRoot.has_value() || pqRoot->root_page_id != manifest.pq_mapping_root_page_id ||
					pqRoot->root_generation != manifest.pq_mapping_root_generation) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found a stale PQ mapping root");
				}
			}
			const size_t nonPqManifestEntries =
					static_cast<size_t>(std::ranges::count_if(manifest.entries, [](const VectorManifestEntry &entry) {
						return entry.storage_kind != VectorStorageKind::VSK_PQ;
					}));
			if (nonPqManifestEntries >= RSS_VECTOR_MANIFEST_MAX_ENTRIES) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training has no Manifest capacity for a PQ generation");
			}
			const size_t maximumPqManifestPages =
					std::min(maximumPages, static_cast<size_t>(RSS_VECTOR_MANIFEST_MAX_ENTRIES) - nonPqManifestEntries);

			const std::vector<BTreePageEntry> forward =
					BTreeIndex(definition->index_id, *btree_pages_, *metadata_)
							.scan(sourceSnapshot, std::nullopt, checkedBoundPlusOne(maximumRecords,
									"relocatable logical Vector training record bound overflowed"),
								  BufferPoolReadHint::BPRH_SEQUENTIAL, request.cancellation);
			if (forward.size() > maximumRecords) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training exceeded its Vector-record bound");
			}
			std::vector<std::pair<uint64_t, uint64_t>> semanticPairs;
			semanticPairs.reserve(forward.size());
			std::vector<DeterministicPqTrainingRecord> records;
			records.reserve(forward.size());
			const size_t rawRecordBytes =
					sizeof(uint64_t) + static_cast<size_t>(definition->vector_dimensions) * sizeof(uint16_t);
			for (const BTreePageEntry &entry: forward) {
				checkVectorTrainingCancellation(request);
				const uint64_t nodeId = decodeCanonicalNodeMappingKey(entry.key);
				const uint64_t vectorId = entry.value_or_child;
				if (vectorId == 0U) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found a zero semantic Vector ID");
				}
				const std::optional<uint64_t> reverseNode = VectorNodeMapping::findNode(
						sourceSnapshot, *btree_pages_, *metadata_, manifest.vector_node_mapping_index_id, vectorId);
				if (!reverseNode.has_value() || *reverseNode != nodeId) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found non-bijective semantic mappings");
				}
				const VectorLocation location = metadata_->findVectorLocation(sourceSnapshot, vectorId);
				const VectorPage page = vector_pages_->read(sourceSnapshot, location.page_id);
				const uint8_t invalidRawPage = static_cast<uint8_t>(page.header().index_id != definition->index_id) |
											   static_cast<uint8_t>(page.header().kind != VectorStorageKind::VSK_RAW) |
											   static_cast<uint8_t>(page.header().page_id != location.page_id) |
											   static_cast<uint8_t>(page.header().record_bytes != rawRecordBytes);
				if (invalidRawPage != 0U) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found an invalid raw Vector page");
				}
				const std::vector<std::byte> raw = page.read(location.slot_id, location.slot_generation);
				if (raw.size() != rawRecordBytes || LittleEndianCodec::readU64(raw, 0U) != vectorId) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found a stale raw Vector record");
				}
				semanticPairs.emplace_back(vectorId, nodeId);
				records.push_back({vectorId, std::vector<std::byte>(raw.begin() + sizeof(uint64_t), raw.end())});
			}
			std::ranges::sort(semanticPairs);
			std::ranges::sort(records, {}, &DeterministicPqTrainingRecord::vector_id);
			if (std::ranges::adjacent_find(semanticPairs, {}, &std::pair<uint64_t, uint64_t>::first) !=
				semanticPairs.end()) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found duplicate semantic Vector IDs");
			}
			const std::vector<BTreePageEntry> reverse =
					BTreeIndex(manifest.vector_node_mapping_index_id, *btree_pages_, *metadata_)
							.scan(sourceSnapshot, std::nullopt, checkedBoundPlusOne(maximumRecords,
									"relocatable logical Vector training mapping bound overflowed"),
								  BufferPoolReadHint::BPRH_SEQUENTIAL, request.cancellation);
			if (reverse.size() != semanticPairs.size()) {
				failVectorTraining(
						FormatError::FERR_INVALID_STATE,
						"relocatable logical Vector training found mismatched semantic mapping cardinalities");
			}
			for (size_t ordinal = 0U; ordinal < reverse.size(); ++ordinal) {
				checkVectorTrainingCancellation(request);
				const uint64_t vectorId = decodeCanonicalVectorMappingKey(reverse[ordinal].key);
				if (reverse[ordinal].value_or_child == 0U ||
					semanticPairs[ordinal] !=
							std::pair<uint64_t, uint64_t>{vectorId, reverse[ordinal].value_or_child}) {
					failVectorTraining(
							FormatError::FERR_INVALID_STATE,
							"relocatable logical Vector training found inconsistent semantic mapping directions");
				}
			}
			if (records.empty()) {
				sourceSnapshot.release();
				return result;
			}

			const uint32_t subspaces = selectPqSubspaceCount(definition->vector_dimensions);
			const uint32_t centroids = static_cast<uint32_t>(std::min<size_t>(UINT8_MAX + 1U, records.size()));
			const DeterministicPqOptions training{definition->vector_dimensions,
												  subspaces,
												  centroids,
												  options_.vector_training_iterations,
												  maximumRecords,
												  std::min(options_.maximum_vector_training_samples, records.size())};
			OfflineVectorBaseRebuildOptions rebuild;
			rebuild.maximum_delta_mapping_pages = options_.maximum_vector_delta_pages;
			rebuild.maximum_delta_overlay_entries = options_.maximum_vector_delta_overlay_entries;
			rebuild.pq.training = training;
			rebuild.pq.codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
			rebuild.pq.codebook.dimensions = training.dimensions;
			rebuild.pq.codebook.subspace_count = training.subspace_count;
			rebuild.pq.codebook.centroid_count = training.centroid_count;
			rebuild.pq.codebook_first_slot = PhysicalSlotId(1U);
			rebuild.pq.codebook_last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
			rebuild.pq.pages = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
								Generation(1U), maximumRecords, maximumPqManifestPages};
			rebuild.pq.mapping.maximum_entries = maximumRecords;
			rebuild.pq.mapping.tree = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()), 0U,
									   maximumRecords, maximumPages};

			const bool hasAnyAdjacencyIdentity = manifest.adjacency_mapping_index_id != 0U ||
												 manifest.adjacency_mapping_root_page_id.isValid() ||
												 manifest.adjacency_mapping_root_generation.isValid();
			const bool hasCompleteAdjacencyIdentity =
					VectorAdjacencyMapping::isMappingIndexId(manifest.adjacency_mapping_index_id) &&
					manifest.adjacency_mapping_root_page_id.isValid() &&
					manifest.adjacency_mapping_root_generation.isValid() && manifest.adjacency_generation.isValid() &&
					manifest.entry_point_vector_id != 0U;
			if ((hasAnyPqIdentity(manifest) && !hasCompletePqIdentity(manifest)) ||
				(hasAnyAdjacencyIdentity && !hasCompleteAdjacencyIdentity)) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found a partial derived Vector generation");
			}

			const bool hasDelta = manifest.delta_mapping_index_id != 0U ||
								  manifest.delta_mapping_root_page_id.isValid() ||
								  manifest.delta_mapping_root_generation.isValid() || manifest.delta_entry_count != 0U;
			const bool hasDeltaIdentity = manifest.delta_mapping_index_id != 0U;
			const bool hasDeltaRootPage = manifest.delta_mapping_root_page_id.isValid();
			const bool hasDeltaRootGeneration = manifest.delta_mapping_root_generation.isValid();
			if (hasDeltaIdentity != hasDeltaRootPage || hasDeltaIdentity != hasDeltaRootGeneration ||
				(hasDeltaIdentity && manifest.delta_entry_count == 0U) ||
				(!hasDeltaIdentity && manifest.delta_entry_count != 0U) ||
				(hasDeltaIdentity && !VectorDeltaMapping::isMappingIndexId(manifest.delta_mapping_index_id))) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found a partial delta generation");
			}
			if (hasDeltaIdentity) {
				const std::optional<IndexRootLocation> deltaRoot =
						metadata_->findIndexRootOptional(sourceSnapshot, manifest.delta_mapping_index_id);
				if (!deltaRoot.has_value() || deltaRoot->root_page_id != manifest.delta_mapping_root_page_id ||
					deltaRoot->root_generation != manifest.delta_mapping_root_generation) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
										   "relocatable logical Vector training found a stale delta mapping root");
				}
			}
			bool reuseVisibleCodebook = false;
			if (!hasDelta && hasCompletePqIdentity(manifest) &&
				VectorCodebookExtentIdCodec::canEncodeCodebookId(manifest.codebook_id)) {
				const VectorCodebookManifest existingCodebook =
						vector_codebooks_->manifest(sourceSnapshot, manifest.codebook_id);
				const VectorCodebookHeader &header = existingCodebook.header;
				if (header.generation == manifest.codebook_generation &&
					header.element_type == rebuild.pq.codebook.element_type &&
					header.dimensions == training.dimensions && header.subspace_count == training.subspace_count &&
					header.centroid_count == training.centroid_count) {
					const ExtentLocation rootExtent = metadata_->findExtentLocation(
							sourceSnapshot, VectorCodebookExtentIdCodec::manifestExtentId(manifest.codebook_id));
					rebuild.pq.codebook.codebook_id = manifest.codebook_id;
					rebuild.pq.codebook.manifest_segment_id = rootExtent.segment_id;
					rebuild.pq.codebook.generation = manifest.codebook_generation;
					rebuild.pq.codebook.data_extents.reserve(existingCodebook.entries.size());
					for (const VectorCodebookManifestEntry &entry: existingCodebook.entries) {
						const ExtentLocation location = metadata_->findExtentLocation(sourceSnapshot, entry.extent_id);
						rebuild.pq.codebook.data_extents.push_back({entry.extent_id, location.segment_id});
					}
					rebuild.pq.pages.codebook_generation = manifest.codebook_generation;
					reuseVisibleCodebook = true;
				}
			}

			if (hasCompletePqIdentity(manifest)) {
				rebuild.pq.mapping_index_id = manifest.pq_mapping_index_id;
				rebuild.pq.mapping.tree.pool_owner_id = manifest.pq_mapping_index_id;
			}
			if (hasCompleteAdjacencyIdentity) {
				if (hasDelta && manifest.adjacency_generation.value() == std::numeric_limits<uint64_t>::max()) {
					failVectorTraining(FormatError::FERR_ARITHMETIC_OVERFLOW,
									   "relocatable logical Vector training exhausted adjacency generation space");
				}
				rebuild.adjacency.mapping_index_id = manifest.adjacency_mapping_index_id;
				rebuild.adjacency.mapping.tree.pool_owner_id = manifest.adjacency_mapping_index_id;
				rebuild.adjacency.adjacency_generation =
						hasDelta ? Generation(manifest.adjacency_generation.value() + 1U)
								 : manifest.adjacency_generation;
			} else {
				rebuild.adjacency.adjacency_generation = Generation(1U);
			}

			const bool needsPqMappingId = rebuild.pq.mapping_index_id == 0U;
			const bool needsAdjacencyMappingId = rebuild.adjacency.mapping_index_id == 0U;
			if (!reuseVisibleCodebook) {
				checkVectorTrainingCancellation(request);
				const uint64_t codebookBytes =
						static_cast<uint64_t>(training.dimensions) * training.centroid_count * sizeof(float);
				if (codebookBytes > std::numeric_limits<size_t>::max()) {
					failVectorTraining(FormatError::FERR_ARITHMETIC_OVERFLOW,
									   "relocatable logical Vector training Codebook exceeds host-addressable memory");
				}
				const size_t dataExtentCount =
						VectorCodebookStore::requiredDataExtentCount(static_cast<size_t>(codebookBytes));
				if (dataExtentCount == std::numeric_limits<size_t>::max()) {
					failVectorTraining(FormatError::FERR_ARITHMETIC_OVERFLOW,
									   "relocatable logical Vector training derived identity count overflowed");
				}
				const Superblock &sourceSuperblock = sourceSnapshot.superblock();
				const uint64_t maximumCodebookPayload = RSS_VECTOR_CODEBOOK_EXTENT_PAYLOAD_MASK;
				const uint8_t exhaustedCodebookIdentity =
						static_cast<uint8_t>(!VectorCodebookExtentIdCodec::canEncodeCodebookId(
								sourceSuperblock.next_logical_page_id)) |
						static_cast<uint8_t>(dataExtentCount > maximumCodebookPayload) |
						static_cast<uint8_t>(
								dataExtentCount != 0U &&
								(dataExtentCount > maximumCodebookPayload ||
								 sourceSuperblock.next_logical_segment_id == 0U ||
								 sourceSuperblock.next_logical_segment_id > maximumCodebookPayload - dataExtentCount));
				if (exhaustedCodebookIdentity != 0U) {
					failVectorTraining(
							FormatError::FERR_ARITHMETIC_OVERFLOW,
							"relocatable logical Vector training exhausted immutable Codebook identity space");
				}
				const std::vector<SegmentId> segmentIds = metadata_->allocateLogicalSegmentIds(
						dataExtentCount + 1U, nextVectorTrainingCheckpoint(*metadata_));
				const size_t mappingIdentityCount =
						static_cast<size_t>(needsPqMappingId) + static_cast<size_t>(needsAdjacencyMappingId);
				const std::vector<PageId> pageIds = metadata_->allocateLogicalPageIds(
						mappingIdentityCount + 1U, nextVectorTrainingCheckpoint(*metadata_));
				const PageId codebookAllocatorId = pageIds.front();
				rebuild.pq.codebook.codebook_id = codebookAllocatorId.value();
				rebuild.pq.codebook.manifest_segment_id = segmentIds.front();
				rebuild.pq.codebook.generation = Generation(1U);
				rebuild.pq.codebook.data_extents.reserve(dataExtentCount);
				for (size_t ordinal = 0U; ordinal < dataExtentCount; ++ordinal) {
					checkVectorTrainingCancellation(request);
					rebuild.pq.codebook.data_extents.push_back(
							{VectorCodebookExtentIdCodec::dataExtentId(segmentIds.at(ordinal + 1U)),
							 segmentIds.at(ordinal + 1U)});
				}
				// The trailing logical IDs are independently typed so a damaged
				// Manifest can never bind PQ data as adjacency data. Codebook extent
				// IDs derive from their dedicated Segment IDs and need no Page-ID
				// reservation, avoiding unused logical identity gaps for large books.
				size_t mappingOrdinal = 1U;
				if (needsPqMappingId) {
					rebuild.pq.mapping_index_id =
							derivedVectorIdentity(RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal++));
					rebuild.pq.mapping.tree.pool_owner_id = rebuild.pq.mapping_index_id;
				}
				if (needsAdjacencyMappingId) {
					rebuild.adjacency.mapping_index_id = derivedVectorIdentity(
							RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal));
					rebuild.adjacency.mapping.tree.pool_owner_id = rebuild.adjacency.mapping_index_id;
				}
			} else if (needsPqMappingId || needsAdjacencyMappingId) {
				checkVectorTrainingCancellation(request);
				const size_t mappingCount =
						static_cast<size_t>(needsPqMappingId) + static_cast<size_t>(needsAdjacencyMappingId);
				const std::vector<PageId> pageIds =
						metadata_->allocateLogicalPageIds(mappingCount, nextVectorTrainingCheckpoint(*metadata_));
				size_t mappingOrdinal = 0U;
				if (needsPqMappingId) {
					rebuild.pq.mapping_index_id =
							derivedVectorIdentity(RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal++));
					rebuild.pq.mapping.tree.pool_owner_id = rebuild.pq.mapping_index_id;
				}
				if (needsAdjacencyMappingId) {
					rebuild.adjacency.mapping_index_id = derivedVectorIdentity(
							RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal));
					rebuild.adjacency.mapping.tree.pool_owner_id = rebuild.adjacency.mapping_index_id;
				}
			}

			rebuild.adjacency.maximum_exact_records =
					std::min(options_.maximum_vector_adjacency_exact_records, maximumRecords);
			rebuild.adjacency.build_mode = records.size() <= rebuild.adjacency.maximum_exact_records
												   ? VectorAdjacencyBuildMode::VABM_EXACT
												   : VectorAdjacencyBuildMode::VABM_CANDIDATE_PRUNED;
			rebuild.adjacency.maximum_candidate_records = maximumRecords;
			rebuild.adjacency.candidate_search_budget =
					std::min(options_.vector_adjacency_candidate_search_budget, maximumRecords);
			rebuild.adjacency.pruning_alpha = options_.vector_adjacency_pruning_alpha;
			rebuild.adjacency.maximum_degree = options_.maximum_vector_adjacency_degree;
			rebuild.adjacency.pages = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									   maximumRecords, maximumPages};
			rebuild.adjacency.mapping.maximum_entries = maximumRecords;
			rebuild.adjacency.mapping.tree = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
											  rebuild.adjacency.mapping_index_id, maximumRecords, maximumPages};
			sourceSnapshot.release();
			checkVectorTrainingCancellation(request);

			VectorAdjacencyPageStore adjacencyPages(data_file_->io(), *metadata_,
													SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
			// Logical ID leases above may have their own durable metadata generation.
			// Compare only the state immediately preceding the derived-generation
			// publisher so a pre-publication rebuild failure stays retryable.
			MetadataSnapshotPin publicationSnapshot = metadata_->openSnapshot();
			const Superblock publicationBase = publicationSnapshot.superblock();
			publicationSnapshot.release();
			OfflineVectorBaseRebuildResult published;
			try {
				published = OfflineVectorBaseRebuild::run(*btree_pages_, *vector_pages_, adjacencyPages,
														  *vector_codebooks_, *vector_manifests_, *metadata_,
														  definition->index_id, records, rebuild, request.cancellation);
			} catch (...) {
				markReopenRequiredIfPublicationAdvanced(publicationBase);
				throw;
			}
			result.vector_count = published.vector_count;
			result.trained = true;
			result.reused_existing_generation = published.already_published;
		}
		return result;
	}

	std::vector<IndexDefinition> RelocatableStorageBackend::readAllIndexDefinitions(const MetadataSnapshotPin &snapshot,
																									size_t maximumDefinitions) const {
		if (maximumDefinitions == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Index definition read requires a positive caller-provided bound");
		}
		const std::vector<IndexDefinition> canonical =
				system_catalog_->readIndexDefinitions(snapshot, maximumDefinitions);
		for (const IndexDefinition &definition: canonical) {
			if (definition.kind != IndexDefinitionKind::IDK_VECTOR &&
				!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable canonical Index definition has no published physical root");
			}
		}
		// The canonical catalog remains authoritative when the definition set is
		// empty; consulting a second registry here could resurrect dropped state.
		return canonical;
	}

	std::vector<ConstraintDefinition>
	RelocatableStorageBackend::readAllConstraintDefinitions(const MetadataSnapshotPin &snapshot,
															size_t maximumDefinitions) const {
		if (maximumDefinitions == 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable constraint definition read requires a positive bound and initialized catalog");
		}
		return system_catalog_->readConstraintDefinitions(snapshot, maximumDefinitions);
	}

	RelocatableStorageIOStats RelocatableStorageBackend::ioStats() const {
		requireOpen();
		return {data_file_->io()->stats(), data_file_->io()->bufferPoolStats()};
	}

	void RelocatableStorageBackend::resetIoStats() {
		requireOpen();
		data_file_->io()->resetStats();
		data_file_->io()->resetBufferPoolStats();
	}

	std::optional<PropertyValue> RelocatableStorageBackend::findConfiguration(std::string_view name) const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		return system_catalog_->findConfiguration(snapshot, name);
	}

	std::vector<LogicalGraphConfiguration> RelocatableStorageBackend::readConfigurations(size_t maximumRecords) const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::vector<NativeConfigurationRecord> records =
				system_catalog_->readConfigurations(snapshot, maximumRecords);
		std::vector<LogicalGraphConfiguration> result;
		result.reserve(records.size());
		for (const NativeConfigurationRecord &record: records) {
			result.push_back({record.name, record.value});
		}
		return result;
	}

	void RelocatableStorageBackend::setConfiguration(std::string_view name, const PropertyValue &value) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot update durable configuration");
		}
		requireNoIndeterminateWrite();
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		SystemCatalogWriteSession session = system_catalog_->beginWrite(transaction);
		session.setConfiguration(name, value);
		session.finish();
		const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable durable configuration lost its native publication fence");
		}
	}

	std::vector<LogicalGraphRuntimeStatistic> RelocatableStorageBackend::runtimeStatistics() const {
		const RelocatableStorageIOStats statistics = ioStats();
		const auto asPropertyValue = [](const uint64_t value) {
			return PropertyValue(static_cast<int64_t>(
					std::min<uint64_t>(value, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))));
		};
		return {
				{"io", "read_bytes", asPropertyValue(statistics.data.read_bytes)},
				{"io", "write_bytes", asPropertyValue(statistics.data.write_bytes)},
				{"io", "reserved_bytes", asPropertyValue(statistics.data.reserved_bytes)},
				{"io", "read_calls", asPropertyValue(statistics.data.read_calls)},
				{"io", "write_calls", asPropertyValue(statistics.data.write_calls)},
				{"io", "reserve_calls", asPropertyValue(statistics.data.reserve_calls)},
				{"io", "sync_calls", asPropertyValue(statistics.data.sync_calls)},
				{"io", "data_sync_calls", asPropertyValue(statistics.data.data_sync_calls)},
				{"io", "superblock_sync_calls", asPropertyValue(statistics.data.superblock_sync_calls)},
				{"buffer_pool", "capacity_bytes", asPropertyValue(statistics.buffer_pool.capacity_bytes)},
				{"buffer_pool", "resident_bytes", asPropertyValue(statistics.buffer_pool.resident_bytes)},
				{"buffer_pool", "pinned_bytes", asPropertyValue(statistics.buffer_pool.pinned_bytes)},
				{"buffer_pool", "dirty_bytes", asPropertyValue(statistics.buffer_pool.dirty_bytes)},
				{"buffer_pool", "hits", asPropertyValue(statistics.buffer_pool.hits)},
				{"buffer_pool", "misses", asPropertyValue(statistics.buffer_pool.misses)},
				{"buffer_pool", "evictions", asPropertyValue(statistics.buffer_pool.evictions)},
				{"buffer_pool", "prefetch_requests", asPropertyValue(statistics.buffer_pool.prefetch_requests)},
				{"buffer_pool", "prefetch_hits", asPropertyValue(statistics.buffer_pool.prefetch_hits)},
				{"buffer_pool", "disk_read_bytes", asPropertyValue(statistics.buffer_pool.disk_read_bytes)},
				{"buffer_pool", "disk_write_bytes", asPropertyValue(statistics.buffer_pool.disk_write_bytes)},
		};
	}

	std::optional<LogicalGraphCardinalityStatistics>
	RelocatableStorageBackend::readLogicalGraphCardinalityStatistics() const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::optional<NativeGraphCardinalityStatistics> statistics =
				system_catalog_->readGraphCardinalityStatistics(snapshot);
		if (!statistics.has_value()) {
			return std::nullopt;
		}
		return LogicalGraphCardinalityStatistics{statistics->node_count, statistics->edge_count};
	}

	namespace {
		std::optional<uint64_t> estimateSnapshotLabels(SystemCatalog &catalog,
			const MetadataSnapshotPin &snapshot, uint64_t nodes, std::span<const std::string> labels) {
			uint64_t estimate = nodes;
			for (const auto &label : labels) {
				if (label.empty()) return std::nullopt;
				const auto token = catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label);
				if (!token) return std::nullopt;
				const auto count = catalog.readTokenCardinality(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *token);
				if (!count || *count > nodes) return std::nullopt;
				estimate = std::min(estimate, *count);
			}
			return estimate;
		}
	}

	std::optional<uint64_t> RelocatableStorageBackend::estimateLogicalNodeCardinality(
			std::span<const std::string> labels) const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const auto statistics = system_catalog_->readGraphCardinalityStatistics(snapshot);
		if (!statistics) return std::nullopt;
		return estimateSnapshotLabels(*system_catalog_, snapshot, statistics->node_count, labels);
	}

	LogicalGraphPlanningStatistics RelocatableStorageBackend::readLogicalPlanningStatistics(
			std::span<const std::vector<std::string>> labels) const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		LogicalGraphPlanningStatistics result;
		result.node_counts.resize(labels.size());
		const auto statistics = system_catalog_->readGraphCardinalityStatistics(snapshot);
		if (!statistics) return result;
		result.graph = LogicalGraphCardinalityStatistics{statistics->node_count, statistics->edge_count};
		std::map<std::string, std::optional<uint64_t>> names;
		std::vector<uint64_t> tokens;
		for (const auto &combination : labels) for (const auto &label : combination)
			if (!label.empty()) names.try_emplace(label, std::nullopt);
		std::vector<std::string> orderedNames;
		orderedNames.reserve(names.size());
		for (const auto &[name, token] : names) orderedNames.push_back(name);
		try {
			const auto resolved = system_catalog_->findSchemaTokens(snapshot,
				SystemCatalogSchemaTokenKind::SCSTK_LABEL, orderedNames);
			for (size_t i = 0U; i < orderedNames.size(); ++i) names.at(orderedNames[i]) = resolved.at(i);
		} catch (...) {
			// A malformed name must not disable unrelated validated inputs.
			for (auto &[name, token] : names) {
				try { token = system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name); }
				catch (...) { }
			}
		}
		for (const auto &[name, token] : names) if (token) tokens.push_back(*token);
		std::sort(tokens.begin(), tokens.end());
		tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
		std::vector<std::optional<uint64_t>> counts;
		try {
			if (!tokens.empty()) counts = system_catalog_->readTokenCardinalities(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, tokens);
		} catch (...) {
			// Preserve unrelated valid estimates if a corrupt token breaks the
			// combined read. This slower path still uses the same pinned snapshot.
			for (size_t i = 0U; i < labels.size(); ++i) {
				try { result.node_counts[i] = estimateSnapshotLabels(*system_catalog_, snapshot, statistics->node_count, labels[i]); }
				catch (...) { }
			}
			return result;
		}
		for (size_t i = 0U; i < labels.size(); ++i) {
			uint64_t estimate = statistics->node_count;
			bool available = true;
			for (const auto &label : labels[i]) {
				const auto found = names.find(label);
				if (found == names.end() || !found->second) { available = false; break; }
				const size_t position = std::lower_bound(tokens.begin(), tokens.end(), *found->second) - tokens.begin();
				const auto count = counts.at(position);
				if (!count || *count > statistics->node_count) { available = false; break; }
				estimate = std::min(estimate, *count);
			}
			if (available) result.node_counts[i] = estimate;
		}
		return result;
	}

	void RelocatableStorageBackend::resetRuntimeStatistics() { resetIoStats(); }

	LogicalGraphIndexDefinition
	RelocatableStorageBackend::createLogicalNodeIndex(const LogicalGraphNodeIndexCreateRequest &request) {
		const std::vector<LogicalGraphIndexDefinition> created =
				createLogicalNodeIndexes(std::span<const LogicalGraphNodeIndexCreateRequest>(&request, 1U));
		if (created.size() != 1U) {
			throw std::logic_error("relocatable logical node Index creation did not return exactly one definition");
		}
		return created.front();
	}

	void RelocatableStorageBackend::rebuildLogicalTokenCardinalityStatistics(const std::function<void()> &checkpoint) {
		requireOpen();
		if (isReadOnly()) throw std::runtime_error("token statistics rebuilding requires a writable backend");
		requireNoIndeterminateWrite();
		if (checkpoint) checkpoint();
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		auto snapshot = metadata_->openSnapshot();
		const Superblock base = snapshot.superblock();
		if (base.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "statistics rebuild checkpoint overflowed");
		}
		const LogSequenceNumber lsn(base.checkpoint_lsn.value() + 1U);
		ExternalBTreeEntrySorter sorter({options_.maximum_online_index_entries,
				options_.online_index_sort_run_bytes, options_.maximum_online_index_sort_runs});
		NativeGraphCardinalityStatistics totals;
		NativeReadSession session; session.bind(snapshot);
		const auto addMember = [&](SystemCatalogSchemaTokenKind kind, uint64_t token, uint64_t entity) {
			auto key = SystemCatalogTokenCardinalityCodec::countKey(kind, token);
			for (int shift = 56; shift >= 0; shift -= 8) key.push_back(static_cast<std::byte>((entity >> shift) & 0xffU));
			sorter.add({std::move(key), entity});
		};
		std::vector<uint64_t> nodeIds;
		nodeIds.reserve(BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
		const auto flush = [&] {
			if (checkpoint) checkpoint();
			const auto labels = native_graph_reads_->readNodeLabelTokensSorted(snapshot, nodeIds, &session);
			for (size_t i = 0U; i < nodeIds.size(); ++i) {
				for (uint64_t token : labels[i]) addMember(SystemCatalogSchemaTokenKind::SCSTK_LABEL, token, nodeIds[i]);
			}
			nodeIds.clear();
		};
		const bool nodesComplete = native_graph_reads_->visitNodeRefsUntil(snapshot,
				options_.maximum_online_index_scan_nodes, [&](const EntityRef &ref) {
					++totals.node_count; nodeIds.push_back(ref.entity_id);
					if (nodeIds.size() == BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES) flush(); return true;
				}, &session);
		if (!nodesComplete) throw std::runtime_error("token statistics Node scan exceeded its bound");
		if (!nodeIds.empty()) flush();
		const bool edgesComplete = native_graph_reads_->visitEdgeRefsUntil(snapshot,
				options_.maximum_online_index_entries, [&](const EntityRef &ref) {
					if (totals.edge_count % BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES == 0U && checkpoint) checkpoint();
					++totals.edge_count;
					addMember(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, ref.type_token, ref.entity_id); return true;
				}, &session);
		if (!edgesComplete) throw std::runtime_error("token statistics Edge scan exceeded its bound");
		const auto sorted = sorter.finish();
		const OrderedBTreeEntrySource statistics = [sorted, totals, checkpoint] {
			auto cursor = sorted();
			return [cursor = std::move(cursor), totals, checkpoint, phase = 0U, scanned = size_t{0U},
					pending = std::optional<BTreePageEntry>{}]() mutable -> std::optional<BTreePageEntry> {
				if (phase == 0U) { ++phase; return BTreePageEntry{SystemCatalogGraphCardinalityCodec::nodeCountKey(),
						SystemCatalogGraphCardinalityCodec::encodeCount(totals.node_count)}; }
				if (phase == 1U) { ++phase; return BTreePageEntry{SystemCatalogGraphCardinalityCodec::edgeCountKey(),
						SystemCatalogGraphCardinalityCodec::encodeCount(totals.edge_count)}; }
				if (phase == 2U) {
					if (!pending) pending = cursor();
					if (pending) {
						if (pending->key.size() != 18U || pending->value_or_child == 0U)
							throw FormatException(FormatError::FERR_INVALID_STATE, "statistics sorter returned an invalid member");
						std::vector<std::byte> key(pending->key.begin(), pending->key.begin() + 10U);
						uint64_t count = 0U;
						do {
							if (scanned++ % BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES == 0U && checkpoint) checkpoint();
							++count; pending = cursor();
						} while (pending && pending->key.size() == 18U && std::equal(key.begin(), key.end(), pending->key.begin()));
						return BTreePageEntry{std::move(key), SystemCatalogGraphCardinalityCodec::encodeCount(count)};
					}
					++phase;
				}
				if (phase == 3U) { ++phase; return BTreePageEntry{SystemCatalogTokenCardinalityCodec::completeKey(
						SystemCatalogSchemaTokenKind::SCSTK_LABEL), 1U}; }
				if (phase == 4U) { ++phase; return BTreePageEntry{SystemCatalogTokenCardinalityCodec::completeKey(
						SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE), 1U}; }
				return std::nullopt;
			};
		};
		OrderedBTreeEntrySource preserved;
		if (metadata_->findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID)) {
			const auto existing = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, *btree_pages_, *metadata_)
					.orderedSource(snapshot, options_.maximum_bulk_edge_index_entries);
			preserved = [existing, checkpoint] {
				auto cursor = existing();
				return [cursor = std::move(cursor), checkpoint, count = size_t{0U}]() mutable -> std::optional<BTreePageEntry> {
					while (auto entry = cursor()) {
						if (count++ % BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES == 0U && checkpoint) checkpoint();
						if (entry->key.size() >= 2U && entry->key[0] == std::byte{13U} &&
								entry->key[1] >= std::byte{1U} && entry->key[1] <= std::byte{6U}) continue;
						return entry;
					}
					return std::nullopt;
				};
			};
		} else {
			preserved = [] {
				return [done = false]() mutable -> std::optional<BTreePageEntry> {
					if (done) return std::nullopt; done = true;
					return BTreePageEntry{SystemCatalogTokenKeyCodec::bootstrapKey(), RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE};
				};
			};
		}
		const auto entries = NativeBulkEdgeCatalogTokenMaterializer::overlayEntries(preserved, statistics);
		const NativeStreamedGraphCatalogIndexPlanningOptions options{PhysicalSlotId(1U),
				PhysicalSlotId(std::numeric_limits<uint64_t>::max()), options_.maximum_bulk_edge_index_entries,
				options_.maximum_online_index_pages};
		auto plan = NativeStreamedGraphCatalogIndexPlanner::plan(*system_catalog_, *metadata_, entries, true, options, snapshot, lsn);
		if (!plan) throw std::logic_error("token statistics rebuild produced no Catalog plan");
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations.push_back(plan->mutation);
		if (checkpoint) checkpoint();
		try {
			static_cast<void>(metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived, lsn,
					[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
						std::span<EntityPageNewPoolBatchMaterialization>, std::span<NativeGraphPropertyPageReplacementMaterialization>,
						std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
					[preparer = plan->preparer](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
						if (batches.size() != 1U) throw std::logic_error("statistics rebuild changed its Catalog partition");
						preparer(batches.front());
					}, {}, {}, {}, {}, {}, {}, {}, base.generation));
		} catch (...) {
			markReopenRequiredIfPublicationAdvanced(base); throw;
		}
	}

	std::vector<uint64_t> RelocatableStorageBackend::ensureLogicalMembershipIndexes(
			const std::function<void()> &checkpoint) {
		if (checkpoint) checkpoint();
		requireOpen();
		if (isReadOnly()) throw std::runtime_error("membership Index creation requires a writable backend");
		requireNoIndeterminateWrite();
		if (options_.maximum_online_index_entries == 0U || options_.maximum_online_index_scan_nodes == 0U ||
				options_.maximum_online_index_pages == 0U || options_.online_index_sort_run_bytes == 0U ||
				options_.maximum_online_index_sort_runs == 0U) {
			throw std::invalid_argument("membership Index creation requires positive work bounds");
		}
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		auto transaction = native_graph_write_planner_->beginTransaction();
		const auto &snapshot = transaction.baseSnapshot();
		const auto existing = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		std::array<IndexDefinition, 2U> definitions;
		std::array<bool, 2U> missing{true, true};
		for (const auto &definition : existing) {
			if (definition.kind != IndexDefinitionKind::IDK_LABEL) continue;
			const size_t ordinal = definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE ? 0U : 1U;
			if (missing[ordinal]) { definitions[ordinal] = definition; missing[ordinal] = false; }
		}
		if (!missing[0] && !missing[1]) return {definitions[0].index_id, definitions[1].index_id};
		const size_t additions = static_cast<size_t>(missing[0]) + static_cast<size_t>(missing[1]);
		if (additions > options_.maximum_index_definitions - existing.size()) {
			throw std::runtime_error("membership Index definitions exceed the configured bound");
		}
		auto catalogSession = system_catalog_->beginWrite(transaction);
		std::array<std::unique_ptr<ExternalBTreeEntrySorter>, 2U> sorters;
		for (size_t ordinal = 0U; ordinal < 2U; ++ordinal) {
			if (!missing[ordinal]) continue;
			auto &definition = definitions[ordinal];
			definition.entity_kind = ordinal == 0U ? IndexDefinitionEntityKind::IDEK_NODE : IndexDefinitionEntityKind::IDEK_EDGE;
			definition.kind = IndexDefinitionKind::IDK_LABEL;
			const std::string base = ordinal == 0U ? "__zyx_node_membership" : "__zyx_edge_membership";
			definition.name = base;
			size_t suffix = 0U;
			while (std::ranges::any_of(existing, [&](const auto &other) { return other.name == definition.name; })) {
				definition.name = base + "_" + std::to_string(++suffix);
			}
			definition = catalogSession.createIndexDefinition(std::move(definition));
			sorters[ordinal] = std::make_unique<ExternalBTreeEntrySorter>(ExternalBTreeEntrySorterOptions{
					options_.maximum_online_index_entries, options_.online_index_sort_run_bytes,
					options_.maximum_online_index_sort_runs});
		}
		NativeGraphCardinalityStatistics scannedCounts;
		NativeReadSession session;
		session.bind(snapshot);
		const auto addMember = [&](size_t ordinal, uint64_t token, uint64_t id) {
			if (token > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				throw FormatException(FormatError::FERR_INVALID_STATE, "membership token exceeds scalar encoding");
			}
			sorters[ordinal]->add({IndexKeyCodec::encodeEntryKey(PropertyValue(static_cast<int64_t>(token)), id), id});
		};
		if (missing[0]) {
			std::vector<uint64_t> ids;
			ids.reserve(BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
			const auto flush = [&] {
				if (checkpoint) checkpoint();
				const auto labels = native_graph_reads_->readNodeLabelTokensSorted(snapshot, ids, &session);
				for (size_t i = 0U; i < ids.size(); ++i) {
					for (uint64_t token : labels[i]) addMember(0U, token, ids[i]);
				}
				ids.clear();
			};
			const bool complete = native_graph_reads_->visitNodeRefsUntil(snapshot,
					options_.maximum_online_index_scan_nodes, [&](const EntityRef &reference) {
						++scannedCounts.node_count;
						ids.push_back(reference.entity_id);
						if (ids.size() == BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES) flush();
						return true;
					}, &session);
			if (!complete) throw std::runtime_error("membership Index source exceeded its scan bound");
			if (!ids.empty()) flush();
		}
		if (missing[1]) {
			const bool complete = native_graph_reads_->visitEdgeRefsUntil(snapshot,
					options_.maximum_online_index_entries, [&](const EntityRef &reference) {
						if (checkpoint) checkpoint();
						++scannedCounts.edge_count;
						if (reference.type_token != 0U) addMember(1U, reference.type_token, reference.entity_id);
						return true;
					}, &session);
			if (!complete) throw std::runtime_error("membership Index source exceeded its scan bound");
		}
		for (size_t ordinal = 0U; ordinal < 2U; ++ordinal) {
			if (!missing[ordinal]) continue;
			if (checkpoint) checkpoint();
			const auto unchecked = sorters[ordinal]->finish();
			OrderedBTreeEntrySource source = unchecked;
			if (checkpoint) source = [unchecked, checkpoint] {
				auto cursor = unchecked();
				return [cursor = std::move(cursor), checkpoint, count = size_t{0U}]() mutable {
					if (count++ % 256U == 0U) checkpoint();
					return cursor();
				};
			};
			const BulkBTreeBuildOptions build{PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
					definitions[ordinal].index_id, options_.maximum_online_index_entries, options_.maximum_online_index_pages};
			transaction.stageScalarIndexCreate(*btree_pages_, BulkBTreeBuilder::planNativeTree(
					*btree_pages_, *metadata_, snapshot, definitions[ordinal].index_id, source, build));
		}
		// Both exhaustive sources proved completion under the same snapshot.
		// Backfill live totals only with this proof, never from high-water IDs.
		if (missing[0] && missing[1] && !system_catalog_->readGraphCardinalityStatistics(snapshot).has_value()) {
			catalogSession.setGraphCardinalityStatistics(scannedCounts);
		}
		catalogSession.finish();
		if (checkpoint) checkpoint();
		if (!commitNativeGraphWriteTransaction(transaction).committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "membership Index creation lost its publication fence");
		}
		return {definitions[0].index_id, definitions[1].index_id};
	}

	LogicalGraphIndexDefinition RelocatableStorageBackend::createLogicalNodeIndexWithCheckpoint(
		const LogicalGraphNodeIndexCreateRequest &request, const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) {
		auto result = createLogicalNodeIndexesWithCheckpoint(std::span(&request, 1U), checkpoint, propertyAdmission);
		return std::move(result.front());
	}

	std::vector<LogicalGraphIndexDefinition> RelocatableStorageBackend::createLogicalNodeIndexes(
			const std::span<const LogicalGraphNodeIndexCreateRequest> requests) {
		return createLogicalNodeIndexesWithCheckpoint(requests, {});
	}

	std::vector<LogicalGraphIndexDefinition>
	RelocatableStorageBackend::createLogicalNodeIndexesWithCheckpoint(std::span<const LogicalGraphNodeIndexCreateRequest> requests,
		const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) {
		if (checkpoint) checkpoint();
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot create a logical Index");
		}
		requireNoIndeterminateWrite();
		if (requests.empty() || requests.size() > options_.maximum_index_definitions ||
			options_.maximum_online_index_entries == 0U || options_.maximum_online_index_scan_nodes == 0U ||
			options_.maximum_online_index_pages == 0U || options_.online_index_sort_run_bytes == 0U ||
			options_.maximum_online_index_sort_runs == 0U) {
			throw std::invalid_argument(
					"relocatable logical node Index batch creation requires bounded nonempty schema input");
		}

		// Validate the complete request set before allocating labels or physical
		// trees, so invalid batches cannot leave a logical schema side effect.
		std::set<std::string, std::less<>> requestedNames;
		std::set<std::pair<std::string, std::vector<std::string>>> requestedSchemas;
		for (const LogicalGraphNodeIndexCreateRequest &request: requests) {
			if (request.label.empty() || request.label.size() > RSS_TOKEN_DEFINITION_MAX_NAME_BYTES ||
				request.property_keys.empty() || request.property_keys.size() > RSS_INDEX_DEFINITION_MAX_PROPERTIES) {
				throw std::invalid_argument(
						"relocatable logical node Index has an invalid label or Property-key sequence");
			}
			std::set<std::string, std::less<>> requestedProperties;
			for (const std::string &property: request.property_keys) {
				if (property.empty() || property.size() > RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES ||
					!requestedProperties.insert(property).second) {
					throw std::invalid_argument("relocatable logical node Index has an invalid Property-key sequence");
				}
			}
			const std::string name =
					request.name.empty() ? defaultNodeIndexName(request.label, request.property_keys) : request.name;
			if (name.empty() || name.size() > RSS_INDEX_DEFINITION_MAX_NAME_BYTES ||
				!requestedNames.insert(name).second ||
				!requestedSchemas.insert({request.label, request.property_keys}).second) {
				throw std::invalid_argument(
						"relocatable logical node Index batch contains a duplicate or invalid schema");
			}
		}

		std::vector<LogicalGraphIndexDefinition> result;
		result.reserve(requests.size());
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		const std::vector<IndexDefinition> existing =
				readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		if (requests.size() > options_.maximum_index_definitions - existing.size()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical node Index batch exceeds the configured definition capacity");
		}

		std::vector<IndexDefinition> definitions;
		std::vector<std::vector<uint32_t>> propertyTokens;
		definitions.reserve(requests.size());
		propertyTokens.reserve(requests.size());
		for (size_t requestIndex = 0U; requestIndex < requests.size(); ++requestIndex) {
			const LogicalGraphNodeIndexCreateRequest &request = requests[requestIndex];
			IndexDefinition definition;
			definition.name =
					request.name.empty() ? defaultNodeIndexName(request.label, request.property_keys) : request.name;
			definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
			definition.kind = request.property_keys.size() == 1U ? IndexDefinitionKind::IDK_PROPERTY
																 : IndexDefinitionKind::IDK_COMPOSITE;
			definition.label_token_id =
					catalogSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, request.label);
			definition.property_names = request.property_keys;
			std::vector<uint32_t> tokens;
			tokens.reserve(request.property_keys.size());
			for (const std::string &property: request.property_keys) {
				tokens.push_back(catalogSession.ensurePropertyToken(property));
			}
			definition = catalogSession.createIndexDefinition(std::move(definition));
			static_cast<void>(IndexDefinitionCodec::encode(definition));
			if (std::ranges::any_of(existing,
									[&](const IndexDefinition &candidate) {
										return candidate.name == definition.name ||
											   sameIndexSchema(candidate, definition.entity_kind, definition.kind,
															   definition.label_token_id, definition.property_names);
									}) ||
				std::ranges::any_of(definitions, [&](const IndexDefinition &candidate) {
					return candidate.name == definition.name ||
						   sameIndexSchema(candidate, definition.entity_kind, definition.kind,
										   definition.label_token_id, definition.property_names);
				})) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable logical node Index name or physical schema already exists");
			}
			result.push_back({definition.index_id,
							  definition.name,
							  logicalIndexKind(definition.kind),
							  request.label,
							  definition.property_names,
							  0U,
							  {}});
			definitions.push_back(std::move(definition));
			propertyTokens.push_back(std::move(tokens));
		}

		// Build every requested index from one bounded Node/label/Property scan.
		// The previous per-definition loop repeated the Entity Map descent and
		// Property Directory merge for each index in a DDL batch, which made a
		// multi-index schema operation scale with the number of definitions rather
		// than with the graph. Each sorter remains independent, so index ordering,
		// entry limits, and external-sort memory bounds are unchanged.
		struct IndexBuildState {
			ExternalBTreeEntrySorter sorter;
			std::vector<std::pair<uint32_t, size_t>> token_order;
			std::vector<uint32_t> sorted_tokens;
			uint64_t indexed_entries = 0U;

			IndexBuildState(ExternalBTreeEntrySorterOptions options, const std::vector<uint32_t> &tokens) :
					sorter(options) {
				token_order.reserve(tokens.size());
				for (size_t tokenIndex = 0U; tokenIndex < tokens.size(); ++tokenIndex) {
					token_order.emplace_back(tokens[tokenIndex], tokenIndex);
				}
				std::ranges::sort(token_order, {}, &std::pair<uint32_t, size_t>::first);
				for (size_t index = 1U; index < token_order.size(); ++index) {
					if (token_order[index - 1U].first == token_order[index].first) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable logical node Index creation resolved duplicate Property tokens");
					}
				}
				sorted_tokens.reserve(token_order.size());
				for (const auto &[token, originalIndex]: token_order) {
					static_cast<void>(originalIndex);
					sorted_tokens.push_back(token);
				}
			}
		};

		std::vector<IndexBuildState> indexStates;
		indexStates.reserve(definitions.size());
		for (size_t definitionIndex = 0U; definitionIndex < definitions.size(); ++definitionIndex) {
			indexStates.emplace_back(
					ExternalBTreeEntrySorterOptions{options_.maximum_online_index_entries,
															 options_.online_index_sort_run_bytes,
															 options_.maximum_online_index_sort_runs},
					propertyTokens[definitionIndex]);
		}

		constexpr size_t ONLINE_INDEX_NODE_BATCH_ENTRIES = 512U;
		std::vector<uint64_t> nodeBatch;
		nodeBatch.reserve(ONLINE_INDEX_NODE_BATCH_ENTRIES);
		NativeReadSession readSession;
		const auto flushNodeBatch = [&]() {
			if (checkpoint) checkpoint();
			if (nodeBatch.empty()) {
				return;
			}
			const std::vector<std::vector<uint64_t>> labels =
					native_graph_reads_->readNodeLabelTokensSorted(snapshot, nodeBatch, &readSession);
			if (labels.size() != nodeBatch.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable logical node Index creation returned an invalid label batch");
			}

			std::vector<std::vector<size_t>> candidatePositions(definitions.size());
			std::vector<uint64_t> propertyNodeIds;
			std::vector<uint32_t> propertyTokensUnion;
			propertyNodeIds.reserve(nodeBatch.size());
			for (size_t definitionIndex = 0U; definitionIndex < definitions.size(); ++definitionIndex) {
				const IndexDefinition &definition = definitions[definitionIndex];
				std::vector<size_t> &positions = candidatePositions[definitionIndex];
				positions.reserve(nodeBatch.size());
				for (size_t position = 0U; position < nodeBatch.size(); ++position) {
					if (std::ranges::binary_search(labels[position], definition.label_token_id)) {
						positions.push_back(position);
						propertyNodeIds.push_back(nodeBatch[position]);
					}
				}
				propertyTokensUnion.insert(propertyTokensUnion.end(), indexStates[definitionIndex].sorted_tokens.begin(),
														 indexStates[definitionIndex].sorted_tokens.end());
			}
			std::ranges::sort(propertyNodeIds);
			propertyNodeIds.erase(std::ranges::unique(propertyNodeIds).begin(), propertyNodeIds.end());
			std::ranges::sort(propertyTokensUnion);
			propertyTokensUnion.erase(std::ranges::unique(propertyTokensUnion).begin(), propertyTokensUnion.end());

			std::vector<std::vector<std::optional<graph::PropertyValue>>> columns;
			if (!propertyNodeIds.empty()) {
				auto admission = makeSelectedPropertyBatchAdmission(propertyAdmission);
				columns = native_graph_reads_->readNodePropertyColumnsSorted(
						snapshot, propertyNodeIds, propertyTokensUnion, &readSession, admission ? &admission : nullptr);
				if (columns.size() != propertyTokensUnion.size() ||
						std::ranges::any_of(columns, [&](const auto &column) {
							return column.size() != propertyNodeIds.size();
						})) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable logical node Index creation returned an invalid Property column batch");
				}
			}

			for (size_t definitionIndex = 0U; definitionIndex < definitions.size(); ++definitionIndex) {
				IndexBuildState &state = indexStates[definitionIndex];
				const IndexDefinition &definition = definitions[definitionIndex];
				const std::vector<uint32_t> &tokens = propertyTokens[definitionIndex];
				for (const size_t nodePosition : candidatePositions[definitionIndex]) {
					const auto propertyPosition = std::ranges::lower_bound(propertyNodeIds, nodeBatch[nodePosition]);
					if (propertyPosition == propertyNodeIds.end() || *propertyPosition != nodeBatch[nodePosition]) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable logical node Index creation lost a candidate ID");
					}
					const size_t row = static_cast<size_t>(propertyPosition - propertyNodeIds.begin());
					std::vector<graph::PropertyValue> values(tokens.size());
					bool complete = true;
					for (size_t sortedIndex = 0U; sortedIndex < state.token_order.size(); ++sortedIndex) {
						const size_t unionIndex = static_cast<size_t>(
								std::ranges::lower_bound(propertyTokensUnion, state.sorted_tokens[sortedIndex]) -
								propertyTokensUnion.begin());
						if (unionIndex >= columns.size() || !columns[unionIndex][row].has_value()) {
							complete = false;
							break;
						}
						values[state.token_order[sortedIndex].second] = *columns[unionIndex][row];
					}
					if (!complete) {
						continue;
					}
					if (state.indexed_entries == options_.maximum_online_index_entries) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable logical node Index exceeds its configured entry bound");
					}
					const std::optional<std::vector<std::byte>> key =
							definition.kind == IndexDefinitionKind::IDK_PROPERTY
									? IndexKeyCodec::tryEncodeEntryKey(values.front(), nodeBatch[nodePosition])
									: IndexKeyCodec::tryEncodeCompositeEntryKey(values, nodeBatch[nodePosition]);
					if (key.has_value()) {
						state.sorter.add({std::move(*key), nodeBatch[nodePosition]});
						++state.indexed_entries;
					}
				}
			}
			nodeBatch.clear();
		};

		const bool scannedAllNodes = native_graph_reads_->visitNodeRefsUntil(
				snapshot, options_.maximum_online_index_scan_nodes,
				[&](const EntityRef &reference) {
					nodeBatch.push_back(reference.entity_id);
					if (nodeBatch.size() == ONLINE_INDEX_NODE_BATCH_ENTRIES) {
						flushNodeBatch();
					}
					return true;
				}, &readSession);
		flushNodeBatch();
		if (!scannedAllNodes) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable logical node Index graph scan exceeded its configured bounded DDL work");
		}

		for (size_t definitionIndex = 0U; definitionIndex < definitions.size(); ++definitionIndex) {
			if (checkpoint) checkpoint();
			const IndexDefinition &definition = definitions[definitionIndex];
			const OrderedBTreeEntrySource uncheckedSource = indexStates[definitionIndex].sorter.finish();
			OrderedBTreeEntrySource source = uncheckedSource;
			if (checkpoint) source = [uncheckedSource, checkpoint] {
				auto cursor = uncheckedSource();
				return [cursor = std::move(cursor), checkpoint, count = size_t{0U}]() mutable {
					if (count++ % 256U == 0U && checkpoint) checkpoint();
					return cursor();
				};
			};
			const BulkBTreeBuildOptions build = {
					PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()), definition.index_id,
					options_.maximum_online_index_entries, options_.maximum_online_index_pages};
			transaction.stageScalarIndexCreate(*btree_pages_,
												 BulkBTreeBuilder::planNativeTree(*btree_pages_, *metadata_, snapshot,
																									 definition.index_id, source, build));
		}
		if (checkpoint) checkpoint();
		catalogSession.finish();
		if (checkpoint) checkpoint();
		const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical node Index batch lost its native publication fence");
		}
		return result;
	}

	LogicalGraphIndexDefinition
	RelocatableStorageBackend::createLogicalNodeVectorIndex(const LogicalGraphNodeVectorIndexCreateRequest &request) {
		return createLogicalNodeVectorIndexWithCheckpoint(request, {});
	}

	LogicalGraphIndexDefinition
	RelocatableStorageBackend::createLogicalNodeVectorIndexWithCheckpoint(const LogicalGraphNodeVectorIndexCreateRequest &request,
		const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) {
		if (checkpoint) checkpoint();
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot create a logical Vector Index");
		}
		requireNoIndeterminateWrite();
		if (request.label.empty() || request.property_key.empty() ||
			request.property_key.size() > RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES || request.dimensions == 0U ||
			request.dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS || options_.maximum_online_index_entries == 0U ||
			options_.maximum_online_index_scan_nodes == 0U || options_.maximum_online_index_pages == 0U) {
			throw std::invalid_argument(
					"relocatable logical Vector Index creation requires bounded nonempty node schema and dimensions");
		}
		const std::string metric = normalizeVectorMetric(request.metric);


		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
		IndexDefinition definition;
		definition.name = request.name.empty() ? defaultNodeVectorIndexName(request.label, request.property_key,
																			request.dimensions, metric)
											   : request.name;
		definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
		definition.kind = IndexDefinitionKind::IDK_VECTOR;
		definition.label_token_id =
				catalogSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, request.label);
		definition.property_names = {request.property_key};
		definition.vector_dimensions = request.dimensions;
		definition.vector_metric = metric;
		const uint32_t propertyToken = catalogSession.ensurePropertyToken(request.property_key);
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		const std::vector<IndexDefinition> existing =
				readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		if (existing.size() >= options_.maximum_index_definitions) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical Vector Index catalog reached its configured definition bound");
		}
		if (std::ranges::any_of(existing, [&](const IndexDefinition &candidate) {
				return candidate.name == definition.name ||
					   sameIndexSchema(candidate, definition.entity_kind, definition.kind, definition.label_token_id,
									   definition.property_names);
			})) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical Vector Index name or physical schema already exists");
		}
		definition = catalogSession.createIndexDefinition(std::move(definition));
		const VectorDistanceMetric distanceMetric = VectorSemanticCodec::metricFor(definition);

		std::vector<NativeGraphVectorBootstrapRecord> records;
		records.reserve(std::min<size_t>(options_.maximum_online_index_entries, 1024U));
		// Vector bootstrap needs one Property column, not complete logical Nodes.
		// Use the same bounded label-reference scan and sorted selected read as
		// scalar indexes so large imports do not decode every owner's root.
		constexpr size_t ONLINE_VECTOR_PROPERTY_BATCH_ENTRIES = 512U;
		std::vector<uint64_t> pendingNodeIds;
		pendingNodeIds.reserve(ONLINE_VECTOR_PROPERTY_BATCH_ENTRIES);
		NativeReadSession readSession;
		const auto flushPropertyBatch = [&]() {
			if (checkpoint) checkpoint();
			if (pendingNodeIds.empty()) {
				return;
			}
			std::ranges::sort(pendingNodeIds);
			pendingNodeIds.erase(std::ranges::unique(pendingNodeIds).begin(), pendingNodeIds.end());
			auto admission = makeSelectedPropertyBatchAdmission(propertyAdmission);
			const std::array<uint32_t, 1U> selectedTokens{propertyToken};
			auto columns = native_graph_reads_->readNodePropertyColumnsSorted(snapshot, pendingNodeIds,
				selectedTokens, &readSession, admission ? &admission : nullptr);
			if (columns.size() != 1U) throw std::runtime_error("vector bootstrap Property column changed cardinality");
			auto values = std::move(columns.front());
			if (values.size() != pendingNodeIds.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable logical Vector Index creation returned an invalid Property batch");
			}
			for (size_t index = 0U; index < pendingNodeIds.size(); ++index) {
				if (!values[index].has_value()) {
					continue;
				}
				if (records.size() == options_.maximum_online_index_entries) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable logical Vector Index exceeds its configured historical Vector bound");
				}
				const std::vector<std::byte> encoded = VectorSemanticCodec::encodeBFloat16ForLogicalWrite(
						values[index].value(), definition.vector_dimensions);
				if (distanceMetric == VectorDistanceMetric::VDM_COSINE &&
						!VectorSemanticCodec::hasNonZeroFiniteL2Norm(encoded)) {
					throw std::invalid_argument(
							"relocatable logical cosine Vector Index cannot include a zero-norm property");
				}
				records.push_back({pendingNodeIds[index], encoded});
			}
			pendingNodeIds.clear();
		};
		const std::array<uint64_t, 1U> labelTokens{definition.label_token_id};
		size_t scannedForCheckpoint = 0U;
		const bool scannedAllNodes = native_graph_reads_->visitNodeRefsWithLabelTokenMatchesUntil(
				snapshot, options_.maximum_online_index_scan_nodes, labelTokens,
				[&](const EntityRef &reference, const bool labelsMatch) {
					if (++scannedForCheckpoint % 256U == 0U && checkpoint) checkpoint();
					if (labelsMatch) {
						pendingNodeIds.push_back(reference.entity_id);
						if (pendingNodeIds.size() == ONLINE_VECTOR_PROPERTY_BATCH_ENTRIES) {
							flushPropertyBatch();
						}
					}
					return true;
				}, &readSession);
		flushPropertyBatch();
		if (!scannedAllNodes) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable logical Vector Index graph scan exceeded its configured bounded DDL work");
		}
		if (checkpoint) checkpoint();
		if (!records.empty()) {
			NativeGraphVectorBootstrapOptions bootstrap;
			bootstrap.first_slot = PhysicalSlotId(1U);
			bootstrap.last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
			bootstrap.maximum_vectors = options_.maximum_online_index_entries;
			bootstrap.maximum_pages = options_.maximum_online_index_pages;
			bootstrap.checkpoint = checkpoint;
			transaction.stageVectorIndexBootstrap(*btree_pages_, *vector_pages_, *vector_manifests_, definition,
												  records, bootstrap);
		}
		if (checkpoint) checkpoint();
		catalogSession.finish();
		if (checkpoint) checkpoint();
		const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical Vector Index lost its native publication fence");
		}
		return {definition.index_id,	 definition.name,			LogicalGraphIndexKind::LGIK_VECTOR,
				request.label,			 definition.property_names, definition.vector_dimensions,
				definition.vector_metric};
	}

	bool RelocatableStorageBackend::dropLogicalIndexByName(std::string_view name) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot drop a logical Index");
		}
		if (name.empty()) {
			throw std::invalid_argument("relocatable logical Index drop requires a nonempty name");
		}
		requireNoIndeterminateWrite();
		bool dropped = false;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			const std::optional<IndexDefinition> canonical = system_catalog_->findIndexDefinitionByName(snapshot, name);
			if (canonical.has_value()) {
				snapshot.release();
				NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
				SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
				// Scalar trees can be retired in the DDL generation itself. Vector
				// state is dependency-ordered and remains for the dedicated reclaimer.
				if (canonical->kind != IndexDefinitionKind::IDK_VECTOR &&
					metadata_->findIndexRootOptional(transaction.baseSnapshot(), canonical->index_id).has_value()) {
					transaction.stageScalarIndexDrop(*btree_pages_, canonical->index_id,
													 options_.maximum_online_index_pages);
				}
				catalogSession.dropIndexDefinition(*canonical);
				catalogSession.finish();
				const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
				if (!committed.committed()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable canonical Index drop lost its native publication fence");
				}
				dropped = true;
				return true;
			}
			// The canonical catalog is authoritative. An absent name means there
			// is no definition to drop; legacy registry trees are never consulted.
			snapshot.release();
		}
		return dropped;
	}

	bool RelocatableStorageBackend::dropLogicalNodeIndexBySchema(std::string_view label, std::string_view propertyKey) {
		requireOpen();
		if (label.empty() || propertyKey.empty()) {
			throw std::invalid_argument("relocatable logical node Index drop requires nonempty label and Property key");
		}
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::optional<uint64_t> token =
				system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label);
		if (!token.has_value()) {
			snapshot.release();
			return false;
		}
		const std::array<std::string, 1U> properties = {std::string(propertyKey)};
		const std::vector<IndexDefinition> definitions =
				readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		const auto found = std::ranges::find_if(definitions, [&](const IndexDefinition &definition) {
			return sameIndexSchema(definition, IndexDefinitionEntityKind::IDEK_NODE, IndexDefinitionKind::IDK_PROPERTY,
								   *token, properties);
		});
		if (found == definitions.end()) {
			return false;
		}
		const std::string name = found->name;
		snapshot.release();
		return dropLogicalIndexByName(name);
	}

	std::vector<LogicalGraphIndexDefinition>
	RelocatableStorageBackend::listLogicalIndexes(size_t maximumIndexes) const {
		requireOpen();
		if (maximumIndexes == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable logical Index list requires a positive result bound");
		}
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::vector<IndexDefinition> definitions = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		std::vector<LogicalGraphIndexDefinition> result;
		result.reserve(definitions.size());
		for (const IndexDefinition &definition: definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
				definition.kind == IndexDefinitionKind::IDK_LABEL) {
				continue;
			}
			if (result.size() == maximumIndexes) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable logical Index list exceeded its result bound");
			}
			if (definition.label_token_id == 0U) {
				result.push_back({definition.index_id,
								  definition.name,
								  logicalIndexKind(definition.kind),
								  {},
								  definition.property_names,
								  definition.vector_dimensions,
								  definition.vector_metric});
				continue;
			}
			const std::optional<SystemCatalogSchemaToken> label = system_catalog_->findSchemaToken(
					snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, definition.label_token_id);
			if (!label.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable logical Index list found an unknown canonical label Token");
			}
			result.push_back({definition.index_id, definition.name, logicalIndexKind(definition.kind), label->name,
							  definition.property_names, definition.vector_dimensions, definition.vector_metric});
		}
		return result;
	}

	LogicalGraphConstraintDefinition
	RelocatableStorageBackend::createLogicalConstraint(const LogicalGraphConstraintCreateRequest &request) {
		return createLogicalConstraintWithCheckpoint(request, {});
	}

	LogicalGraphConstraintDefinition
	RelocatableStorageBackend::createLogicalConstraintWithCheckpoint(const LogicalGraphConstraintCreateRequest &request,
		const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) {
		if (checkpoint) checkpoint();
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot create a logical constraint");
		}
		requireNoIndeterminateWrite();
		if (request.name.empty() || request.name.size() > RSS_CONSTRAINT_DEFINITION_MAX_NAME_BYTES ||
			request.label.empty() || request.property_keys.empty() ||
			request.property_keys.size() > RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES ||
			options_.maximum_constraint_definitions == 0U) {
			throw std::invalid_argument("relocatable logical constraint creation requires a bounded complete schema");
		}
		for (const std::string &property: request.property_keys) {
			if (property.empty() || property.size() > RSS_CONSTRAINT_DEFINITION_MAX_PROPERTY_NAME_BYTES) {
				throw std::invalid_argument("relocatable logical constraint has an invalid Property key");
			}
		}
		ConstraintDefinition definition;
		definition.name = request.name;
		definition.entity_kind = constraintDefinitionEntityKind(request.entity_kind);
		definition.kind = constraintDefinitionKind(request.kind);
		definition.property_names = request.property_keys;
		definition.property_type = constraintDefinitionPropertyType(request.property_type);

		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
		const SystemCatalogSchemaTokenKind schemaKind =
				definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
						? SystemCatalogSchemaTokenKind::SCSTK_LABEL
						: SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
		definition.schema_token_id = catalogSession.ensureSchemaToken(schemaKind, request.label);
		std::map<std::string, uint32_t, std::less<>> requestedPropertyTokens;
		for (const std::string &property: definition.property_names) {
			requestedPropertyTokens.emplace(property, catalogSession.ensurePropertyToken(property));
		}

		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		const std::vector<ConstraintDefinition> existing =
				readAllConstraintDefinitions(snapshot, options_.maximum_constraint_definitions);
		if (existing.size() >= options_.maximum_constraint_definitions) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical constraint catalog reached its configured definition bound");
		}

		std::vector<ConstraintDefinition> validationDefinitions = existing;
		validationDefinitions.push_back(definition);
		PageBackedConstraintMutationAdapter validationAdapter(
				snapshot, *native_graph_reads_, *btree_pages_, *metadata_,
				[this](const MetadataSnapshotPin &pinned, uint32_t token) {
					const std::optional<std::string> name = system_catalog_->findPropertyName(pinned, token);
					return name.value_or(std::string{});
				},
				[this](const MetadataSnapshotPin &pinned, std::string_view name) {
					const std::optional<uint64_t> token =
							system_catalog_->findSchemaToken(pinned, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
					return token.value_or(0U);
				},
				[this](const MetadataSnapshotPin &pinned, std::string_view name) {
					const std::optional<uint64_t> token = system_catalog_->findSchemaToken(
							pinned, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, name);
					return token.value_or(0U);
				}, selectConstraintProperties(validationDefinitions, [&](std::string_view name) -> std::optional<uint32_t> {
					const auto staged = requestedPropertyTokens.find(name);
					return staged != requestedPropertyTokens.end() ? std::optional<uint32_t>{staged->second}
						: system_catalog_->findPropertyToken(snapshot, name);
				}), checkpoint, propertyAdmission);
		ConstraintMutationValidator validator(
				validationAdapter.makeAdapter(),
				{options_.maximum_online_index_entries, options_.maximum_constraint_support_collisions});
		static_cast<void>(validator.validate(existing, {}, std::span(&definition, 1U)));

		NativeConstraintSupportTreeBuildOptions supportTree;
		if (requiresConstraintSupportTree(definition)) {
			if (options_.maximum_online_index_entries == 0U || options_.maximum_online_index_pages == 0U ||
				options_.online_index_sort_run_bytes == 0U || options_.maximum_online_index_sort_runs < 2U) {
				throw std::invalid_argument(
						"relocatable unique logical constraint creation requires bounded support-tree resources");
			}
			supportTree = {PhysicalSlotId(1U),
						   PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
						   options_.maximum_online_index_entries,
						   options_.maximum_online_index_pages,
						   options_.online_index_sort_run_bytes,
						   options_.maximum_online_index_sort_runs,
						   options_.maximum_constraint_support_collisions};
		}
		supportTree.checkpoint = checkpoint;
		supportTree.property_admission = propertyAdmission;
		NativeConstraintDefinitionWriter::stageCreate(
				transaction, *system_catalog_, catalogSession, *btree_pages_, *metadata_, definition,
				*native_graph_reads_, supportTree,
				[this, &requestedPropertyTokens](const MetadataSnapshotPin &pinned,
												 std::string_view name) -> std::optional<uint32_t> {
					const auto staged = requestedPropertyTokens.find(name);
					if (staged != requestedPropertyTokens.end()) {
						return staged->second;
					}
					return system_catalog_->findPropertyToken(pinned, name);
				});
		if (checkpoint) checkpoint();
		catalogSession.finish();
		if (checkpoint) checkpoint();
		const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical constraint lost its native publication fence");
		}
		return logicalConstraintDefinition(definition, request.label);
	}

	bool RelocatableStorageBackend::dropLogicalConstraintByName(std::string_view name) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot drop a logical constraint");
		}
		if (name.empty()) {
			throw std::invalid_argument("relocatable logical constraint drop requires a nonempty name");
		}
		requireNoIndeterminateWrite();
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		const std::optional<ConstraintDefinition> canonical =
				system_catalog_->findConstraintDefinitionByName(transaction.baseSnapshot(), name);
		if (canonical.has_value()) {
			SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
			NativeConstraintDefinitionWriter::stageDrop(transaction, *system_catalog_, catalogSession, *btree_pages_,
														*metadata_, *canonical, options_.maximum_online_index_pages);
			catalogSession.finish();
			const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
			if (!committed.committed()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable canonical constraint drop lost its native publication fence");
			}
			return true;
		}
		transaction.rollback();
		return false;
	}

	std::vector<LogicalGraphConstraintDefinition>
	RelocatableStorageBackend::listLogicalConstraints(size_t maximumConstraints) const {
		requireOpen();
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::vector<ConstraintDefinition> definitions =
				readAllConstraintDefinitions(snapshot, maximumConstraints);
		std::vector<LogicalGraphConstraintDefinition> result;
		result.reserve(definitions.size());
		for (const ConstraintDefinition &definition: definitions) {
			const SystemCatalogSchemaTokenKind schemaKind =
					definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
							? SystemCatalogSchemaTokenKind::SCSTK_LABEL
							: SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
			const std::optional<SystemCatalogSchemaToken> schema =
					system_catalog_->findSchemaToken(snapshot, schemaKind, definition.schema_token_id);
			if (!schema.has_value() || schema->name.empty()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable logical constraint list found an unknown native schema Token");
			}
			result.push_back(logicalConstraintDefinition(definition, schema->name));
		}
		return result;
	}

	ScalarIndexTreeReclaimReport
	RelocatableStorageBackend::reclaimObsoleteScalarIndexTrees(const ScalarIndexTreeReclaimOptions &options,
															   const MetadataPublishOptions &publishOptions) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot reclaim obsolete scalar Index trees");
		}
		requireNoIndeterminateWrite();
		ScalarIndexTreeReclaimReport report;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			const Superblock publicationBase = snapshot.superblock();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable scalar Index-tree reclamation cannot advance the checkpoint LSN");
			}
			const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			ScalarIndexTreeReachabilityReclaimer reclaimer(*btree_pages_, *system_catalog_, *vector_manifests_,
														   *metadata_);
			try {
				report = reclaimer.reclaim(options, checkpointLsn, publishOptions);
			} catch (...) {
				markReopenRequiredIfPublicationAdvanced(publicationBase);
				throw;
			}
		}
		return report;
	}

	VectorIndexStateReclaimReport
	RelocatableStorageBackend::reclaimObsoleteVectorIndexState(const VectorIndexStateReclaimOptions &options,
															   const MetadataPublishOptions &publishOptions) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot reclaim obsolete Vector Index state");
		}
		requireNoIndeterminateWrite();
		VectorIndexStateReclaimReport report;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			const Superblock publicationBase = snapshot.superblock();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable Vector Index reclamation cannot advance the checkpoint LSN");
			}
			const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			VectorAdjacencyPageStore adjacency(data_file_->io(), *metadata_,
											   SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
			VectorIndexStateReclaimer reclaimer(*physical_, *slotted_pages_, *btree_pages_, *vector_pages_, adjacency,
												*vector_manifests_, *system_catalog_, *metadata_);
			try {
				report = reclaimer.reclaim(options, checkpointLsn, publishOptions);
			} catch (...) {
				markReopenRequiredIfPublicationAdvanced(publicationBase);
				throw;
			}
		}
		return report;
	}

	LargeValueReachabilityReclaimReport
	RelocatableStorageBackend::reclaimUnreachableLargeValues(const LargeValueReachabilityReclaimOptions &options,
															 const MetadataPublishOptions &publishOptions) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot reclaim unreachable LargeValues");
		}
		requireNoIndeterminateWrite();
		LargeValueReachabilityReclaimReport report;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			const Superblock publicationBase = snapshot.superblock();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable LargeValue reclamation cannot advance the checkpoint LSN");
			}
			const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			LargeValueReachabilityReclaimer reclaimer(*large_values_, *slotted_pages_, *metadata_);
			try {
				report = reclaimer.reclaim(options, checkpointLsn, publishOptions);
			} catch (...) {
				markReopenRequiredIfPublicationAdvanced(publicationBase);
				throw;
			}
		}
		return report;
	}

	void RelocatableStorageBackend::pauseMaintenance() { requireMaintenance().pause(); }

	void RelocatableStorageBackend::resumeMaintenance() { requireMaintenance().resume(); }

	bool RelocatableStorageBackend::nativeMaintenanceEnabled() const {
		if (maintenance_ == nullptr) {
			return false;
		}
		return maintenance_->metrics().loop.state == RelocatableMaintenanceLoopState::VMLS_RUNNING;
	}

	void RelocatableStorageBackend::setNativeMaintenanceEnabled(const bool enabled) {
		if (enabled) {
			requireNoIndeterminateWrite();
		}
		RelocatableMaintenanceService &maintenance = requireMaintenance();
		const RelocatableMaintenanceLoopState state = maintenance.metrics().loop.state;
		if (enabled) {
			if (state == RelocatableMaintenanceLoopState::VMLS_RUNNING) {
				return;
			}
			if (state != RelocatableMaintenanceLoopState::VMLS_PAUSED) {
				throw std::logic_error("relocatable maintenance cannot be enabled outside a clean paused state");
			}
			maintenance.resume();
			return;
		}
		if (state == RelocatableMaintenanceLoopState::VMLS_PAUSED ||
			state == RelocatableMaintenanceLoopState::VMLS_FAULTED) {
			return;
		}
		if (state != RelocatableMaintenanceLoopState::VMLS_RUNNING) {
			throw std::logic_error("relocatable maintenance cannot be disabled outside an active lifecycle state");
		}
		maintenance.pause();
	}

	size_t RelocatableStorageBackend::cancelQueuedMaintenance() { return requireMaintenance().cancelQueuedTasks(); }

	uint64_t RelocatableStorageBackend::enqueuePropertyPageCompaction(SegmentId source, SegmentId destination,
																	  uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueuePropertyPageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueBTreePageCompaction(SegmentId source, SegmentId destination,
																   uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueBTreePageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueVectorPageCompaction(SegmentId source, SegmentId destination,
																	uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueVectorPageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueVectorManifestCompaction(SegmentId source, SegmentId destination,
																		uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueVectorManifestCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueEntityPageCompaction(SegmentId source, SegmentId destination,
																	uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueEntityPageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueLargeValuePageCompaction(SegmentId source, SegmentId destination, uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueLargeValuePageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueLargeValueReachabilityReclamation(
			const LargeValueReachabilityReclaimOptions &options) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueLargeValueReachabilityReclamation(options);
	}

	uint64_t RelocatableStorageBackend::enqueueCodebookExtentRelocation(uint64_t codebookId, ExtentId extentId,
																		PhysicalSlotId firstDestination,
																		PhysicalSlotId lastDestination) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueCodebookExtentRelocation(codebookId, extentId, firstDestination,
																	lastDestination);
	}

	RelocatableMaintenanceMetrics RelocatableStorageBackend::maintenanceMetrics() const {
		return requireMaintenance().metrics();
	}

	std::exception_ptr RelocatableStorageBackend::maintenanceFailure() const {
		return requireMaintenance().lastFailure();
	}

	RelocatableIntegrityReport RelocatableStorageBackend::verifyIntegrity() const {
		requireOpen();
		const RelocatableIntegrityReport report = RelocatableIntegrityScanner(data_file_->io(), *metadata_).scan();
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		verifyConstraintSupportTrees(snapshot);
		return report;
	}

	void RelocatableStorageBackend::verifyConstraintSupportTrees(const MetadataSnapshotPin &snapshot) const {
		if (options_.maximum_constraint_definitions == 0U || options_.maximum_online_index_entries == 0U ||
			options_.maximum_online_index_entries == std::numeric_limits<size_t>::max()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable native constraint support-tree integrity verification requires finite "
								  "positive bounds");
		}
		const std::vector<ConstraintDefinition> definitions =
				readAllConstraintDefinitions(snapshot, options_.maximum_constraint_definitions);
		for (const ConstraintDefinition &definition: definitions) {
			if (!requiresConstraintSupportTree(definition))
				continue;
			const std::optional<IndexRootLocation> root =
					metadata_->findIndexRootOptional(snapshot, definition.constraint_id);
			if (!root.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable integrity verification found an active uniqueness constraint "
									  "without a support-tree root");
			}
			const SystemCatalogSchemaTokenKind schemaKind =
					definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
							? SystemCatalogSchemaTokenKind::SCSTK_LABEL
							: SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
			const std::optional<SystemCatalogSchemaToken> schema =
					system_catalog_->findSchemaToken(snapshot, schemaKind, definition.schema_token_id);
			if (!schema.has_value() || schema->name.empty()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable integrity verification found a constraint with an unknown native schema Token");
			}
			std::vector<uint32_t> propertyTokens;
			propertyTokens.reserve(definition.property_names.size());
			bool propertyKeysAssigned = true;
			for (const std::string &name: definition.property_names) {
				const std::optional<uint32_t> token = system_catalog_->findPropertyToken(snapshot, name);
				if (!token.has_value()) {
					propertyKeysAssigned = false;
					break;
				}
				propertyTokens.push_back(*token);
			}
			std::map<std::vector<std::byte>, uint64_t> expectedEntries;
			std::set<std::vector<std::byte>> semanticKeys;
			bool scopedEntityExists = false;
			const auto add = [&](uint64_t entityId, const std::vector<PropertyCollectionRootEntry> &properties) {
				scopedEntityExists = true;
				if (!propertyKeysAssigned) {
					return;
				}
				std::vector<PropertyValue> values;
				values.reserve(propertyTokens.size());
				for (const uint32_t token: propertyTokens) {
					const auto entry =
							std::ranges::lower_bound(properties, token, {}, &PropertyCollectionRootEntry::key_token);
					const PropertyValue *value =
							entry == properties.end() || entry->key_token != token ? nullptr : &entry->value;
					if (value == nullptr || value->getType() == PropertyType::NULL_TYPE) {
						if (definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"relocatable integrity verification found a Node KEY entity with a null Property");
						}
						return;
					}
					values.push_back(*value);
				}
				const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
				if (!semanticKeys.insert(canonical).second) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable integrity verification found duplicate active uniqueness semantics");
				}
				const std::vector<std::byte> entry = ConstraintSupportTree::entryKey(canonical, entityId);
				if (!expectedEntries.emplace(entry, entityId).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable integrity verification found duplicate constraint support-tree "
										  "entry identities");
				}
			};
			size_t scannedEntities = 0U;
			const auto count = [&scannedEntities, this]() {
				if (++scannedEntities > options_.maximum_online_index_entries) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable native constraint support-tree scan exceeded its Entity bound");
				}
			};
			const size_t scanBound = checkedBoundPlusOne(options_.maximum_online_index_entries,
					"relocatable integrity verification scan bound overflowed");
			if (definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE) {
				static_cast<void>(
						native_graph_reads_->visitNodesUntil(snapshot, scanBound, [&](const PageBackedGraphNode &node) {
							count();
							if (std::ranges::find(node.label_token_ids, definition.schema_token_id) !=
								node.label_token_ids.end()) {
								add(node.node_id, node.properties);
							}
							return true;
						}));
			} else {
				static_cast<void>(
						native_graph_reads_->visitEdgesUntil(snapshot, scanBound, [&](const PageBackedGraphEdge &edge) {
							count();
							if (edge.type_id == definition.schema_token_id) {
								add(edge.edge_id, edge.properties);
							}
							return true;
						}));
			}
			if (!propertyKeysAssigned && definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY &&
				scopedEntityExists) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable integrity verification found a Node KEY whose Property-key Token is absent");
			}
			const std::vector<BTreePageEntry> actual =
					BTreeIndex(definition.constraint_id, *btree_pages_, *metadata_)
							.scan(snapshot, std::nullopt, checkedBoundPlusOne(options_.maximum_online_index_entries,
									"relocatable integrity verification index bound overflowed"));
			if (actual.size() > options_.maximum_online_index_entries || actual.size() != expectedEntries.size()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable integrity verification found a constraint support-tree entry-count mismatch");
			}
			auto expected = expectedEntries.begin();
			for (const BTreePageEntry &entry: actual) {
				if (expected == expectedEntries.end() || entry.key != expected->first ||
					entry.value_or_child != expected->second ||
					ConstraintSupportTree::entryEntityId(entry.key) != entry.value_or_child) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable integrity verification found a constraint support-tree entry mismatch");
				}
				++expected;
			}
		}
	}

	void RelocatableStorageBackend::requireOpen() const {
		if (!isOpen()) {
			throw std::logic_error("relocatable backend is not open");
		}
	}

	RelocatableMaintenanceService &RelocatableStorageBackend::requireMaintenance() {
		requireOpen();
		if (maintenance_ == nullptr) {
			throw std::logic_error("relocatable backend was not opened with background maintenance enabled");
		}
		return *maintenance_;
	}

	const RelocatableMaintenanceService &RelocatableStorageBackend::requireMaintenance() const {
		requireOpen();
		if (maintenance_ == nullptr) {
			throw std::logic_error("relocatable backend was not opened with background maintenance enabled");
		}
		return *maintenance_;
	}

	void RelocatableStorageBackend::requireOwnedSnapshot(const RelocatableReadSnapshot &snapshot) const {
		requireOpen();
		if (snapshot.backend_ != this) {
			throw std::invalid_argument("relocatable backend snapshot does not belong to this open backend");
		}
	}

	std::string RelocatableStorageBackend::resolveNodeLabelToken(const MetadataSnapshotPin &snapshot,
																 uint64_t tokenId) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid Node-label resolver state");
		}
		const std::optional<SystemCatalogSchemaToken> token =
				system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, tokenId);
		if (!token.has_value() || token->name.empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown Node-label Token");
		}
		return token->name;
	}

	std::string RelocatableStorageBackend::resolveRelationshipTypeToken(const MetadataSnapshotPin &snapshot,
																		uint64_t tokenId) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid relationship-type resolver state");
		}
		auto token = system_catalog_->findSchemaToken(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, tokenId);
		if (!token.has_value() || token->name.empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown relationship-type Token");
		}
		return std::move(token->name);
	}
	std::string RelocatableStorageBackend::resolveRelationshipTypeTokenWithNameAdmission(const MetadataSnapshotPin &snapshot,
																		uint64_t tokenId, const std::function<void(size_t)> &admission) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid relationship-type resolver state");
		}
		auto token = system_catalog_->findSchemaToken(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, tokenId, admission);
		if (!token.has_value() || token->name.empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown relationship-type Token");
		}
		return std::move(token->name);
	}

	std::string RelocatableStorageBackend::resolvePropertyKey(const MetadataSnapshotPin &snapshot,
															  uint32_t tokenId) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid Property-key resolver state");
		}
		const std::optional<std::string> resolved = system_catalog_->findPropertyName(snapshot, tokenId);
		if (!resolved.has_value() || resolved->empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown Property-key Token: " +
										  std::to_string(tokenId));
		}
		return *resolved;
	}

	std::string RelocatableStorageBackend::resolvePropertyKeyWithNameAdmission(const MetadataSnapshotPin &snapshot,
															  uint32_t tokenId, const std::function<void(size_t)> &admission) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid Property-key resolver state");
		}
		auto resolved = system_catalog_->findPropertyName(snapshot, tokenId, admission);
		if (!resolved.has_value() || resolved->empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown Property-key Token: " +
										  std::to_string(tokenId));
		}
		return std::move(*resolved);
	}

	std::optional<uint32_t> RelocatableStorageBackend::resolvePropertyToken(const MetadataSnapshotPin &snapshot,
																							std::string_view name) const {
		if (name.empty()) {
			return std::nullopt;
		}
		debug::PerfTrace::addValue("native_graph.read.property_token_resolution", 1);
		return system_catalog_->findPropertyToken(snapshot, name);
	}

	std::optional<uint64_t> RelocatableStorageBackend::resolveNodeLabelName(const MetadataSnapshotPin &snapshot,
																						  std::string_view name) const {
		if (name.empty()) {
			return std::nullopt;
		}
		debug::PerfTrace::addValue("native_graph.read.label_token_resolution", 1);
		return system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
	}

	void RelocatableStorageBackend::releaseResources() noexcept {
		if (maintenance_ != nullptr) {
			try {
				maintenance_->stop();
			} catch (...) {
				std::terminate();
			}
		}
		maintenance_.reset();
		native_graph_mutation_executor_.reset();
		native_logical_graph_reads_.reset();
		native_graph_reads_.reset();
		native_graph_write_planner_.reset();
		vector_manifests_.reset();
		vector_pages_.reset();
		properties_.reset();
		large_values_.reset();
		native_labels_.reset();
		native_entities_.reset();
		system_catalog_.reset();
		btree_pages_.reset();
		slotted_pages_.reset();
		vector_codebooks_.reset();
		physical_.reset();
		metadata_.reset();
		data_file_.reset();
	}

} // namespace graph::storage
