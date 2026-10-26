/**
 * @file RelocatableStorageBackend.cpp
 * @brief File-owning implementation of the relocatable logical read backend.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "storage/relocatable/backend/QueryScratchDiskBudget.hpp"
#include "storage/relocatable/backend/AdmittedMutationInput.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "query/execution/ProcessResourceDomains.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedPmrObject.hpp"
#include "graph/utils/OwnedVector.hpp"
#include "graph/utils/OwnedAssociativeContainers.hpp"
#include "graph/storage/LogicalGraphTransactionMemoryProvider.hpp"
#include "graph/storage/relocatable/entity/ScanReadProof.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <type_traits>
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
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
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

		[[nodiscard]] std::string nativeOpenedFileIdentity(file_handle_t handle) {
			if (handle == INVALID_FILE_HANDLE) { // Callers identify an already opened native file, including physical read-only handles.
				throw std::logic_error("relocatable resource domain requires a native file handle");
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

        [[nodiscard]] std::shared_ptr<MetadataSnapshotCoordination> metadataFileCoordination(
            const std::string &key, const std::shared_ptr<std::pmr::memory_resource> &owner) {
            static std::mutex mutex;
            static std::map<std::string, std::weak_ptr<MetadataSnapshotCoordination>> domains;
            std::lock_guard lock(mutex);
            for (auto it = domains.begin(); it != domains.end();)
                if (it->second.expired()) it = domains.erase(it); else ++it;
            if (auto existing = domains[key].lock()) return existing;
            auto created = makeMetadataSnapshotCoordination(owner); domains[key] = created; return created;
        }

		[[nodiscard]] std::shared_ptr<std::mutex>
		nativeBulkSessionProcessMutex(file_handle_t handle) {
			static std::mutex registryMutex;
			static std::map<std::string, std::weak_ptr<std::mutex>> registry;
			// Key by the opened file identity so hard-link and symlink aliases share a lease.
			const std::string key = nativeOpenedFileIdentity(handle);
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
		[[nodiscard]] static std::unique_ptr<OwnedRelocatableBackendFile> create(const std::filesystem::path &path, uint64_t maximumFileBytes) {
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
													  StorageIOAccessMode::SIOAM_READ_WRITE, maximumFileBytes);
			// Cache admission follows opened-file and durable database identity binding.
			return result;
		}

		[[nodiscard]] static std::unique_ptr<OwnedRelocatableBackendFile> open(const std::filesystem::path &path,
																			   bool writable, uint64_t maximumFileBytes) {
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
															   : StorageIOAccessMode::SIOAM_READ_ONLY, maximumFileBytes);
            if (writable && result->io_->fileSize() > maximumFileBytes) {
                throw std::system_error(std::make_error_code(std::errc::no_space_on_device),
                    "relocatable existing database exceeds its configured file byte limit");
            }
			// Cache admission follows opened-file and durable database identity binding.
			return result;
		}

		~OwnedRelocatableBackendFile() {
			io_.reset();
			static_cast<void>(portable_close_rw(write_fd_));
			static_cast<void>(portable_close(read_fd_));
			stream_.reset();
		}

		[[nodiscard]] const std::shared_ptr<StorageIO> &io() const { return io_; }
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
        void installVerifiedScanRoot(VerifiedScanRoot root) { verified_scan_.emplace(std::move(root)); }
        [[nodiscard]] const VerifiedScanRoot *verifiedScanRoot() const {
            return verified_scan_ ? &*verified_scan_ : nullptr;
        }
#endif

	private:
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
        std::optional<VerifiedScanRoot> verified_scan_;
#endif
		OwnedRelocatableBackendFile() = default;

		std::shared_ptr<std::fstream> stream_;
		std::shared_ptr<StorageIO> io_;
		file_handle_t write_fd_ = INVALID_FILE_HANDLE;
		file_handle_t read_fd_ = INVALID_FILE_HANDLE;
	};

	namespace {

		constexpr uint64_t RSS_LOGICAL_GRAPH_FENCE_FORMAT_TOKEN = RSS_HYBRID_PROPERTY_FORMAT_ENABLED
            ? UINT64_C(0x564E4558544C4749) : UINT64_C(0x564E4558544C4748);
		using LogicalGraphFenceRootMember = PhysicalPageRef Superblock::*;
		constexpr std::array<LogicalGraphFenceRootMember, LogicalGraphMutationFence::GRAPH_STATE_ROOT_COUNT>
				RSS_LOGICAL_GRAPH_FENCE_ROOT_MEMBERS = {
						&Superblock::node_entity_directory_root,	 &Superblock::edge_entity_directory_root,
						&Superblock::entity_property_directory_root, &Superblock::large_value_manifest_directory_root,
						&Superblock::vector_location_directory_root, &Superblock::vector_manifest_directory_root,
						&Superblock::state_directory_root, &Superblock::hybrid_property_descriptor_root,
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

		class NativeQueryScratchSession final : public LogicalQueryScratchSession, public utils::OwnedPmrObject {
		public:
			NativeQueryScratchSession(RelocatableStorageBackend &backend,std::unique_ptr<NativeBulkRunStore> store,
				std::shared_ptr<NativeBulkSessionLease> lease,std::function<void()> refund,uint64_t owner,size_t bytes,size_t fragments,
				detail::QueryScratchDiskBudget::Session disk, std::shared_ptr<std::pmr::memory_resource> memory,
                std::weak_ptr<StorageIO> io, size_t cleanupBytes)
				:backend_(backend),store_(std::move(store)),lease_(std::move(lease)),refund_(std::move(refund)),owner_(owner),maximum_bytes_(bytes),maximum_fragments_(fragments),locations_(utils::OwnedPmrAllocator<std::pair<const uint64_t, NativeBulkRunFragmentLocation>>(std::move(memory))),disk_(std::move(disk)),io_(std::move(io)),cleanup_bytes_(cleanupBytes) {}
			~NativeQueryScratchSession() override {
				try {finish();} catch(...) {backend_.requireReopenAfterNativeBulkStagingFailure();releasePermit();}
			}
			uint64_t write(std::span<const std::byte> bytes) override {
				requireActive();
				if(bytes.empty() || bytes.size()>maximumFragmentBytes() || locations_.size()>=maximum_fragments_ ||
					RSS_SEGMENT_SIZE>maximum_bytes_-used_bytes_)
					throw std::runtime_error("query scratch exceeded its fragment or disk bound");
                std::vector<std::byte> payload(bytes.begin(), bytes.end());
				// Install bookkeeping before IO so a post-publication allocation failure
				// cannot lose the only live cleanup route.
				auto [position,inserted]=locations_.emplace(0U,NativeBulkRunFragmentLocation{});
				if(!inserted)throw std::logic_error("query scratch session cannot publish concurrently");
				try {disk_.acquire(RSS_SEGMENT_SIZE);}
				catch(...) {locations_.erase(position);throw;}
				try {auto gate=backend_.acquireNativePublicationGate();position->second=store_->writeOpaquePayload(owner_,std::move(payload),backend_.nextNativeBulkScratchCheckpointLsn());}
                catch (const NativeBulkRunCapacityDenied &) {
                    locations_.erase(position);
                    disk_.release(RSS_SEGMENT_SIZE);
                    throw;
                }
				catch(...) {locations_.erase(position);backend_.requireReopenAfterNativeBulkStagingFailure();throw;}
				const uint64_t id=position->second.segment_id.value();auto node=locations_.extract(position);node.key()=id;locations_.insert(std::move(node));
				used_bytes_+=RSS_SEGMENT_SIZE;return id;
			}
			std::vector<std::byte> read(uint64_t id) override {
				requireActive();const auto position=locations_.find(id);if(position==locations_.end())throw std::invalid_argument("query scratch handle is stale or foreign");
				const auto reader=store_->openOpaquePayloadReader();return reader(position->second);
			}
			void release(uint64_t id) override {
				requireActive();auto position=locations_.find(id);if(position==locations_.end())throw std::invalid_argument("query scratch release handle is stale or foreign");
                auto cleanup = openCleanupWorkspace();
				try {auto gate=backend_.acquireNativePublicationGate();store_->release(position->second,backend_.nextNativeBulkScratchCheckpointLsn());}
				catch(...) {backend_.requireReopenAfterNativeBulkStagingFailure();throw;}
				locations_.erase(position);used_bytes_-=RSS_SEGMENT_SIZE;disk_.release(RSS_SEGMENT_SIZE);
			}
			void finish() override {
				if(!refund_)return;
				if(backend_.hasIndeterminateWriteOutcome() && !locations_.empty())throw std::logic_error("query scratch cleanup requires writable recovery");
                auto cleanup = locations_.empty() ? nullptr : openCleanupWorkspace();
				while(!locations_.empty()) {
					std::array<NativeBulkRunFragmentLocation,64U> batch;size_t count=0U;
					for(const auto &[id,location]:locations_){if(count==batch.size())break;batch[count++]=location;}
					try {auto gate=backend_.acquireNativePublicationGate();store_->releaseBatch(std::span(batch).first(count),backend_.nextNativeBulkScratchCheckpointLsn());}
					catch(...) {backend_.requireReopenAfterNativeBulkStagingFailure();throw;}
					for(size_t i=0;i<count;++i)locations_.erase(locations_.begin());used_bytes_-=count*RSS_SEGMENT_SIZE;
					disk_.release(count*RSS_SEGMENT_SIZE);
				}
				releasePermit();
			}
			size_t maximumFragmentBytes() const override {return store_->maximumFragmentBytes();}
        private:
            std::unique_ptr<RelocatableMaintenanceWorkspace> openCleanupWorkspace() const {
                // Cleanup must not compete with the query's full allowance or
                // CACHE credit. Admit the protected grant before publication.
                auto io = io_.lock();
                if (!io) throw std::logic_error("query scratch cleanup lost its open file");
                auto account = std::make_shared<query::execution::detail::ResourceAccount>(
                    backend_.queryResourceDomain(), query::execution::detail::ResourceClass::RCK_MAINTENANCE, cleanup_bytes_);
                auto memory = std::make_shared<query::execution::detail::PrepaidMemoryResource>(
                    std::move(account), cleanup_bytes_, std::pmr::new_delete_resource());
                return std::unique_ptr<RelocatableMaintenanceWorkspace>(
                    new (memory) RelocatableMaintenanceWorkspace(std::move(io), memory));
            }
			void requireActive() const {if(!refund_)throw std::logic_error("query scratch session is finished");if(backend_.hasIndeterminateWriteOutcome())throw std::logic_error("query scratch requires database reopen after uncertain IO");}
			void releasePermit(){if(refund_){disk_.close();auto refund=std::move(refund_);refund();lease_.reset();}}
			RelocatableStorageBackend &backend_;std::unique_ptr<NativeBulkRunStore> store_;std::shared_ptr<NativeBulkSessionLease> lease_;std::function<void()> refund_;
			uint64_t owner_;size_t maximum_bytes_,maximum_fragments_,used_bytes_=0U;std::map<uint64_t,NativeBulkRunFragmentLocation, std::less<uint64_t>, utils::OwnedPmrAllocator<std::pair<const uint64_t, NativeBulkRunFragmentLocation>>> locations_;
			detail::QueryScratchDiskBudget::Session disk_;
            std::weak_ptr<StorageIO> io_;
            size_t cleanup_bytes_;
		};

#include "backend_runtime/LogicalTransaction.inc"

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

        void validateDatabaseFileLimit(uint64_t maximumBytes) {
            if (maximumBytes == 0U || maximumBytes >= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
                throw std::invalid_argument("relocatable database requires a finite addressable file byte limit");
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

        [[nodiscard]] std::shared_ptr<query::execution::detail::PrepaidMemoryResource> mutationWriteWorkspace(
                const std::shared_ptr<query::execution::detail::DatabaseResourceBudget> &domain) {
            using namespace query::execution::detail;
            // Private COW/B+Tree decode must not fall back to the snapshot's
            // CACHE owner. Prepay outside the publication gate using the existing
            // WRITE allowance. Root admission already protects maintenance and
            // subtracts every staged/concurrent owner; do not add an arbitrary
            // smaller COW limit that rejects an otherwise admitted rebuild.
            const size_t maximum = domain->classMaximum(ResourceClass::RCK_WRITE);
            if (!maximum) throw ResourceAdmissionDenied();
            auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, maximum);
            const size_t available = account->available();
            // Logical-ID capability nodes use a separate WRITE owner; leave
            // their bounded publication metadata room before taking the gate.
            constexpr size_t capabilityHeadroom = 4096U;
            if (available <= capabilityHeadroom) throw ResourceAdmissionDenied();
            // Keep concurrent transaction staging separate from the private
            // publication workspace (at most 96MiB of the default 128MiB class).
            const size_t stagingHeadroom = (std::min)(size_t{32U * 1024U * 1024U}, available / 4U);
            if (available - stagingHeadroom <= capabilityHeadroom) throw ResourceAdmissionDenied();
            const size_t capacity = available - stagingHeadroom - capabilityHeadroom;
            return std::make_shared<PrepaidMemoryResource>(std::move(account), capacity, std::pmr::new_delete_resource());
        }

        [[nodiscard]] std::shared_ptr<query::execution::MaterializedByteBudget> ddlWriteWorkspace(
                const std::shared_ptr<query::execution::detail::DatabaseResourceBudget> &domain) {
            const auto maximum = domain->classMaximum(query::execution::detail::ResourceClass::RCK_WRITE);
            if (maximum == 0U) throw query::execution::detail::ResourceAdmissionDenied();
            auto account = std::make_shared<query::execution::detail::ResourceAccount>(
                domain, query::execution::detail::ResourceClass::RCK_WRITE, maximum);
            // Charge actual requests rather than monopolizing the whole WRITE class.
            return std::make_shared<query::execution::MaterializedByteBudget>(maximum,
                std::move(account), std::pmr::new_delete_resource());
        }

        // A stack-only sizing pass rejects oversized names before any string heap
        // request. The filling pass writes into one pre-admitted exact-size value.
        struct DecimalNamePart {
            std::array<char, std::numeric_limits<size_t>::digits10 + 1U> bytes{};
            size_t size;
            explicit DecimalNamePart(size_t value) {
                const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
                if (result.ec != std::errc{}) throw std::length_error("index name decimal overflow");
                size = static_cast<size_t>(result.ptr - bytes.data());
            }
            [[nodiscard]] std::string_view view() const { return {bytes.data(), size}; }
        };

        template<class Parts>
        [[nodiscard]] utils::OwnedStdString boundedIndexName(Parts parts) {
            size_t bytes = 0U;
            parts([&](std::string_view part) {
                if (part.size() > RSS_INDEX_DEFINITION_MAX_NAME_BYTES - bytes)
                    throw std::invalid_argument("logical Index default name exceeds the durable name bound; supply a shorter explicit name");
                bytes += part.size();
            });
            utils::OwnedStdString result;
            result.assign(bytes, '\0');
            size_t offset = 0U;
            parts([&](std::string_view part) {
                std::copy(part.begin(), part.end(), result.data() + offset);
                offset += part.size();
            });
            return result;
        }

        [[nodiscard]] utils::OwnedStdString defaultNodeIndexName(std::string_view label,
                std::span<const std::string> propertyNames) {
            if (label.empty() || propertyNames.empty())
                throw std::invalid_argument("logical node Index requires a label and at least one Property key");
            return boundedIndexName([&](auto emit) {
                emit("index_node_"); emit(DecimalNamePart(label.size()).view()); emit("_"); emit(label);
                for (const auto &property : propertyNames) {
                    emit("_"); emit(DecimalNamePart(property.size()).view()); emit("_"); emit(property);
                }
            });
        }

        [[nodiscard]] utils::OwnedStdString normalizeVectorMetric(std::string_view metric) {
            if (metric.empty() || metric.size() > RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES)
                throw std::invalid_argument("logical Vector Index requires a bounded nonempty distance metric");
            utils::OwnedStdString normalized;
            normalized.assign(metric.size(), '\0');
            std::transform(metric.begin(), metric.end(), normalized.begin(), [](unsigned char byte) {
                return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte - 'A' + 'a' : byte);
            });
            if (normalized == "inner_product" || normalized == "innerproduct") normalized = "ip";
            if (normalized != "l2" && normalized != "ip" && normalized != "cosine")
                throw std::invalid_argument("logical Vector Index requires metric l2, ip, or cosine");
            return normalized;
        }

        [[nodiscard]] utils::OwnedStdString defaultNodeVectorIndexName(std::string_view label,
                std::string_view property, uint32_t dimensions, std::string_view metric) {
            return boundedIndexName([&](auto emit) {
                emit("index_vector_node_"); emit(DecimalNamePart(label.size()).view()); emit("_"); emit(label);
                emit("_"); emit(DecimalNamePart(property.size()).view()); emit("_"); emit(property);
                emit("_"); emit(DecimalNamePart(dimensions).view()); emit("_"); emit(metric);
            });
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
		logicalConstraintDefinition(const ConstraintDefinition &definition, std::string_view label) {
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
        if constexpr (RSS_SCAN_PATH_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                "scan images require explicit offline construction and whole-file admission");
		if (options.open_mode != OpenMode::OPEN_CREATE_NEW_FILE &&
			options.open_mode != OpenMode::OPEN_CREATE_OR_OPEN_FILE) {
			throw std::invalid_argument("relocatable native initialization requires a create-capable open mode");
		}
		validateBufferPoolCapacity(options.buffer_pool_bytes);
        validateDatabaseFileLimit(options.maximum_database_file_bytes);
        if (options.maximum_reclaim_workspace_bytes == 0U || options.maximum_integrity_workspace_bytes == 0U)
            throw std::invalid_argument("native maintenance workspace limits must be positive");
		const std::filesystem::path dataPath = canonicalNewPath(options.data_path, "database");

		std::unique_ptr<OwnedRelocatableBackendFile> data;
		bool created = false;
		try {
			data = OwnedRelocatableBackendFile::create(dataPath, options.maximum_database_file_bytes);
			created = true;
			Superblock initial;
			initial.database_id = DatabaseId(generateDatabaseId());
			initial.generation = Generation(1U);
			initial.checkpoint_lsn = LogSequenceNumber(1U);
			initial.flags = 0U;
			SuperblockCodec::validate(initial);
            auto owner = query::execution::detail::ProcessResourceDomains::defaultOwner();
            auto domain = owner->domain(nativeOpenedFileIdentity(data->io()->getWriteFd()) + ":" +
                                        std::to_string(initial.database_id.value()));
            auto fileLimit = owner->fileLimit(domain, options.maximum_database_file_bytes);
            auto cacheMemory = owner->cacheGrant(domain);
            auto controlMemory = owner->controlGrant(domain);
            auto logicalIdMemory = owner->logicalIdGrant(domain);
            data->io()->enableBufferPool(options.buffer_pool_bytes, cacheMemory);
			auto metadata = RelocatableMetadataStore::initializeNewComplete(data->io(), initial, cacheMemory, controlMemory, logicalIdMemory);
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

    std::shared_ptr<StorageIO> RelocatableStorageBackend::scanSourceIO() const { return data_file_->io(); }

    const VerifiedScanRoot *RelocatableStorageBackend::verifiedScanRoot(const MetadataSnapshotPin &snapshot) const {
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
        if (data_file_ && metadata_) {
            const auto *root = data_file_->verifiedScanRoot();
            if (root && root->matches(*metadata_, *data_file_->io(), snapshot)) return root;
        }
#else
        static_cast<void>(snapshot);
#endif
        return nullptr;
    }

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
        validateDatabaseFileLimit(options_.maximum_database_file_bytes);
        if (options_.maximum_reclaim_workspace_bytes == 0U || options_.maximum_integrity_workspace_bytes == 0U)
            throw std::invalid_argument("native maintenance workspace limits must be positive");

		const std::filesystem::path dataPath = canonicalExistingPath(options_.data_path, "database");

		// Keep every later reopen on the exact identity checked for regular-file semantics.
		options_.data_path = dataPath;

		const bool writable = !isReadOnly();
		auto data = OwnedRelocatableBackendFile::open(dataPath, writable, options_.maximum_database_file_bytes);
		if (!StorageFormatProbe::inspect(*data->io()).isRelocatable()) {
			throw std::runtime_error("relocatable backend refuses a non-complete relocatable database file");
		}
		const Superblock recovered = SuperblockFile(data->io()).recover();
        if (RSS_SCAN_PATH_FORMAT_ENABLED && (recovered.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING))
            throw FormatException(FormatError::FERR_INVALID_STATE, "cannot open an unfinished scan image");
		const auto identityHandle = data->io()->getWriteFd() != INVALID_FILE_HANDLE
			? data->io()->getWriteFd() : data->io()->readFd_;
		auto resourceDomain = query::execution::detail::ProcessResourceDomains::defaultOwner()->domain(
			nativeOpenedFileIdentity(identityHandle) + ":" + std::to_string(recovered.database_id.value()));
        auto fileLimit = writable ? query::execution::detail::ProcessResourceDomains::defaultOwner()->fileLimit(
            resourceDomain, options_.maximum_database_file_bytes) : nullptr;
        auto cacheMemory = query::execution::detail::ProcessResourceDomains::defaultOwner()->cacheGrant(resourceDomain);
        auto controlMemory = query::execution::detail::ProcessResourceDomains::defaultOwner()->controlGrant(resourceDomain);
        auto logicalIdMemory = writable ? query::execution::detail::ProcessResourceDomains::defaultOwner()->logicalIdGrant(resourceDomain) : nullptr;
        data->io()->enableBufferPool(options_.buffer_pool_bytes, cacheMemory);
        auto coordination = metadataFileCoordination(nativeOpenedFileIdentity(identityHandle) + ":" +
            std::to_string(recovered.database_id.value()), controlMemory);
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
        if (writable) {
            // Authenticate complete semantics before writable recovery can
            // change any allocator control record. The writer is verified again
            // below after recovery, against its own store and final I/O epoch.
            auto readOnlyMetadata = RelocatableMetadataStore::openReadOnly(data->io(), cacheMemory,
                controlMemory, logicalIdMemory, coordination);
            const size_t bytes = (std::min)(options_.maximum_integrity_workspace_bytes, size_t{7U * 1024U * 1024U});
            auto account = std::make_shared<query::execution::detail::ResourceAccount>(resourceDomain,
                query::execution::detail::ResourceClass::RCK_MAINTENANCE, bytes);
            auto workspace = std::make_shared<query::execution::MaterializedByteBudget>(bytes, account);
            static_cast<void>(VerifiedScanRoot::verifyComplete(data->io(), *readOnlyMetadata, workspace, controlMemory));
            if (options_.verify_integrity_on_open)
                static_cast<void>(RelocatableIntegrityScanner(data->io(), *readOnlyMetadata, workspace).scan());
        }
#endif
        auto metadata = writable ? RelocatableMetadataStore::open(data->io(), cacheMemory, controlMemory, logicalIdMemory, coordination)
                                 : RelocatableMetadataStore::openReadOnly(data->io(), cacheMemory, controlMemory, logicalIdMemory, coordination);


		try {
			data_file_ = std::move(data);
			resource_domain_ = std::move(resourceDomain);
            database_file_limit_ = std::move(fileLimit);
            native_cache_memory_ = std::move(cacheMemory);
			metadata_ = std::move(metadata);
			physical_ = std::make_unique<PhysicalSegmentAllocator>(data_file_->io(), *metadata_);
			if (writable) {
                // Orphan discovery/reclamation is startup maintenance; its
                // transient catalog decoding must not borrow exhausted CACHE.
                utils::OwnedAllocationScope recoveryScratch(controlMemory);
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
					auto queryOptions=scratchOptions;queryOptions.scratch_kind=SegmentKind::SGK_QUERY_SCRATCH;
					MetadataSnapshotPin queryRecoverySnapshot=metadata_->openSnapshot();
					if(queryRecoverySnapshot.superblock().checkpoint_lsn.value()==std::numeric_limits<uint64_t>::max())throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,"query scratch recovery cannot advance its checkpoint");
					const auto queryRecoveryLsn=LogSequenceNumber(queryRecoverySnapshot.superblock().checkpoint_lsn.value()+1U);queryRecoverySnapshot.release();
					NativeBulkRunStore(*physical_,*metadata_,queryOptions).reclaimUnpublishedOnOpen(queryRecoveryLsn);
					detail::QueryScratchDiskBudget::processBudget()->recovered(nativeOpenedFileIdentity(data_file_->io()->getWriteFd()));

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
															options_.maximum_vector_codebook_cache_entries, native_cache_memory_);
			// A normal open follows the published roots and must not scan every data
			// pool. A writable allocator reconciles its selected pool before frame
			// reuse; routed reads still authenticate each current owner header.
            // Native publication owns recovery and frame reuse under its typed
            // metadata proof. Ordinary allocator attachment must not reconcile
            // native pools before that publication acquires its writer.
            const bool nativeDescriptor = metadata_->openSnapshot().superblock().hybrid_property_descriptor_root.isValid();
			const SharedPagePoolOpenMode poolOpenMode = isReadOnly() || nativeDescriptor
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
				graphWriteOptions.property_roots.adaptive_row_chunks = options_.adaptive_property_row_chunks;
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
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
            const size_t scanWorkspaceBytes = (std::min)(options_.maximum_integrity_workspace_bytes, size_t{7U * 1024U * 1024U});
            auto scanAccount = std::make_shared<query::execution::detail::ResourceAccount>(resource_domain_,
                query::execution::detail::ResourceClass::RCK_MAINTENANCE, scanWorkspaceBytes);
            auto scanWorkspace = std::make_shared<query::execution::MaterializedByteBudget>(scanWorkspaceBytes, scanAccount);
            data_file_->installVerifiedScanRoot(VerifiedScanRoot::verifyComplete(data_file_->io(), *metadata_,
                scanWorkspace, controlMemory));
            verifyConstraintSupportTrees(metadata_->openSnapshot());
            if (options_.verify_integrity_on_open) static_cast<void>(verifyIntegrity());
#else
			if (options_.verify_integrity_on_open) {
				static_cast<void>(verifyIntegrity());
			}
#endif
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
                    const auto definition = system_catalog_->findIndexDefinitionById(snapshot, indexId);
                    if (!definition || definition->kind != IndexDefinitionKind::IDK_VECTOR) {
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
                    // Startup validates the complete schema/root set. A recovery
                    // cycle validates only the route it visits, using canonical
                    // ID/name/semantic checks rather than copying all definitions.
					size_t visitedMetadataPages = 0U;
					result.exhausted = metadata_->visitVectorManifestLocationsAfter(
							snapshot, afterIndexId, maximumMetadataPages,
							[this, &snapshot, &result](const VectorManifestLocation &location) {
                                const auto definition = system_catalog_->findIndexDefinitionById(snapshot, location.index_id);
                                if (!definition || definition->kind != IndexDefinitionKind::IDK_VECTOR) {
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
                                return false;
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
                },
                [this]() -> std::unique_ptr<RelocatableMaintenanceWorkspace> {
                    try {
                        const size_t bytes = options_.maximum_reclaim_workspace_bytes;
                        auto account = std::make_shared<query::execution::detail::ResourceAccount>(
                            resource_domain_, query::execution::detail::ResourceClass::RCK_MAINTENANCE, bytes);
                        auto memory = std::make_shared<query::execution::detail::PrepaidMemoryResource>(
                            std::move(account), bytes, std::pmr::new_delete_resource());
                        return std::unique_ptr<RelocatableMaintenanceWorkspace>(
                            new (memory) RelocatableMaintenanceWorkspace(data_file_->io(), memory));
                    } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                        return {};
                    } catch (const std::bad_alloc &) {
                        return {};
                    }
                });
		// Validate every persisted Vector route before a worker can inspect it.
		MetadataSnapshotPin maintenanceSnapshot = metadata_->openSnapshot();
		utils::OwnedSet<uint64_t> knownVectorIndexes;
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
		{std::lock_guard lock(query_scratch_registry_mutex_);if(query_scratch_sessions_!=0U)
			throw std::logic_error("cannot close the native backend while query scratch sessions are active");}

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
        // This private backend entry publishes typed catalog/index builders.
        // The transaction and metadata writer independently exclude graph/field
        // changes before accepting schema-only native-layout preservation.
        transaction.native_schema_coordinated_ = true;
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

        // Transaction wrapper metadata belongs to the WRITE memory class even
        // in read-only mode; that classification grants no disk-write permission.
        // Only wrapper/issued-ID allocation and release use this parent allocator,
        // outside publication gates. Other staging/COW need prepaid workspaces.
        const auto maximum = query::execution::detail::ProcessResourceDomains::databaseDefaults()
            .classes[static_cast<size_t>(query::execution::detail::ResourceClass::RCK_WRITE)];
        auto account = std::make_shared<query::execution::detail::ResourceAccount>(
            resource_domain_, query::execution::detail::ResourceClass::RCK_WRITE, maximum);
        auto memory = std::make_shared<query::execution::MaterializedByteBudget>(
            maximum, std::move(account), std::pmr::new_delete_resource());
        static_assert(alignof(RelocatableLogicalGraphTransaction) <= alignof(std::max_align_t));

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
				return std::unique_ptr<LogicalGraphTransaction>(new (memory) RelocatableLogicalGraphTransaction(
                    *this, readOnly, std::move(logicalSnapshot), expectedFence, baseMetadataGeneration, std::move(rawSnapshot), memory));
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
		auto workspaceGate = query::execution::detail::ProcessResourceDomains::defaultOwner()->lockWriteWorkspace(resource_domain_);
        auto workspace = mutationWriteWorkspace(resource_domain_);
        utils::OwnedAllocationScope allocation(workspace);
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
		auto workspaceGate = query::execution::detail::ProcessResourceDomains::defaultOwner()->lockWriteWorkspace(resource_domain_);
        auto workspace = mutationWriteWorkspace(resource_domain_);
        utils::OwnedAllocationScope allocation(workspace);
        const NativeGraphLogicalIdLease lease = metadata_->reserveNativeGraphLogicalIds(0U, edgeCount);
		return {{}, {lease.edges}};
	}

	void RelocatableStorageBackend::abandonLogicalGraphCreationIds(
			const NativeGraphLogicalIdReservation &reservation) noexcept {
        abandonLogicalGraphCreationIds(reservation.node_ranges, reservation.edge_ranges);
    }

    void RelocatableStorageBackend::abandonLogicalGraphCreationIds(
            std::span<const NativeGraphLogicalIdLeaseRange> nodes,
            std::span<const NativeGraphLogicalIdLeaseRange> edges) noexcept {
		if (metadata_ != nullptr) {
			metadata_->abandonNativeGraphLogicalIdReservation(nodes, edges);
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

	std::unique_ptr<LogicalQueryScratchSession> RelocatableStorageBackend::openNativeQueryScratchSession(size_t bytes,size_t fragments) {
        return openNativeQueryScratchSession(bytes, fragments, {});
    }

    std::unique_ptr<LogicalQueryScratchSession> RelocatableStorageBackend::openNativeQueryScratchSession(
            size_t bytes, size_t fragments, std::shared_ptr<query::execution::detail::ResourceAccount> queryAccount) {
		requireOpen();requireNoIndeterminateWrite();if(isReadOnly() || !options_.query_scratch_enabled)return nullptr;
		if(bytes<RSS_SEGMENT_SIZE || fragments==0U || fragments>4096U)throw std::invalid_argument("query scratch requires finite disk and handle bounds");
		bytes=std::min({bytes,size_t{128}*1024U*1024U,options_.maximum_database_query_scratch_bytes});
		if(bytes<RSS_SEGMENT_SIZE)throw std::invalid_argument("database query scratch disk quota is smaller than one segment");
        // Admit bounded handle metadata before acquiring the file lease or
        // publishing the scratch owner. The entire grant remains charged while
        // a finished session object is retained. Payload/IO workspaces are separate.
        const size_t grantBytes = 4096U + fragments * 256U;
        auto account = std::move(queryAccount);
        if (account && (!account->belongsTo(*resource_domain_) ||
                account->resourceClass() != query::execution::detail::ResourceClass::RCK_QUERY))
            throw std::invalid_argument("scratch memory requires its database's QUERY account");
        if (!account) account = std::make_shared<query::execution::detail::ResourceAccount>(
            resource_domain_, query::execution::detail::ResourceClass::RCK_QUERY, grantBytes);
        auto memory = std::make_shared<query::execution::detail::PrepaidMemoryResource>(
            std::move(account), grantBytes, std::pmr::new_delete_resource());
        static_assert(alignof(NativeQueryScratchSession) <= alignof(std::max_align_t));
		auto disk=detail::QueryScratchDiskBudget::processBudget()->open(
			nativeOpenedFileIdentity(data_file_->io()->getWriteFd()),options_.maximum_database_query_scratch_bytes,bytes);
		std::shared_ptr<NativeBulkSessionLease> lease;
		{std::lock_guard lock(query_scratch_registry_mutex_);
			if(!query_scratch_lease_){auto exclusive=tryAcquireNativeBulkSessionLease();if(!exclusive)throw std::runtime_error("query scratch is unavailable while another scratch owner holds the file lease");query_scratch_lease_=std::move(exclusive);}
			lease=query_scratch_lease_;++query_scratch_sessions_;
		}
		auto refund=[this]{std::lock_guard lock(query_scratch_registry_mutex_);if(--query_scratch_sessions_==0U)query_scratch_lease_.reset();};
		try {
			uint64_t owner;
			{auto gate=acquireNativePublicationGate();owner=metadata_->allocateLogicalNodeId(nextNativeBulkScratchCheckpointLsn());}
			auto options=NativeBulkRunStoreOptions{PhysicalSlotId(1U),PhysicalSlotId(std::numeric_limits<uint64_t>::max()),RSS_SEGMENT_SIZE-RSS_ENVELOPE_BYTES,64U,8U,SegmentKind::SGK_QUERY_SCRATCH};
			auto store=std::make_unique<NativeBulkRunStore>(*physical_,*metadata_,options);
			return std::unique_ptr<LogicalQueryScratchSession>(new (memory) NativeQueryScratchSession(*this,std::move(store),std::move(lease),refund,owner,bytes,fragments,std::move(disk),memory,data_file_->io(),options_.maximum_reclaim_workspace_bytes));
		}catch(...){refund();throw;}
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
																  options_.maximum_vector_codebook_cache_entries, native_cache_memory_);
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
		graphWriteOptions.property_roots.adaptive_row_chunks = options_.adaptive_property_row_chunks;
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

	LogicalGraphMutationBatchResult RelocatableStorageBackend::executeNativeBlindNodePropertyReplacements(
		const LogicalGraphMutationBatch &batch, const RelocatableReadSnapshot &source,
		const LogicalGraphMutationFence &expected) {
		requireOwnedSnapshot(source);
		if (makeLogicalGraphMutationFence(source.superblock()) != expected ||
			batch.property_replacements.empty() || batch.property_replacements.size() > 64U ||
			!batch.node_creates.empty() || !batch.edge_creates.empty() || !batch.node_deletes.empty() ||
			!batch.edge_deletes.empty() || !batch.node_label_replacements.empty() || batch.creation_identity_plan)
			throw std::invalid_argument("blind Property rebase requires a bounded native replacement-only source");
		// Dependency storage is a fixed stack array. Absence is a dependency too.
		std::array<std::optional<EntityPropertyLocation>,64U> dependencies;
		for (size_t i=0;i<batch.property_replacements.size();++i) {
			const auto &entity=batch.property_replacements[i].entity;
			if (entity.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
				entity.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED || entity.value==0U)
				throw std::invalid_argument("blind Property rebase requires persisted Node owners");
			if (!metadata_->findNodePageLocationOptional(source.pin_,entity.value))
				throw LogicalGraphMutationConflict("blind Property rebase source Node is absent");
			dependencies[i]=metadata_->findEntityPropertyLocationOptional(source.pin_,{EntityPropertyOwnerKind::EPOK_NODE,entity.value});
		}
		const auto validate=[&](const MetadataSnapshotPin &latest) {
			for(size_t i=0;i<batch.property_replacements.size();++i) {
				const auto id=batch.property_replacements[i].entity.value;
				if (!metadata_->findNodePageLocationOptional(latest,id) ||
					metadata_->findEntityPropertyLocationOptional(latest,{EntityPropertyOwnerKind::EPOK_NODE,id}) != dependencies[i])
					throw LogicalGraphMutationConflict("blind Property rebase owner changed or was deleted");
			}
		};
		// The executor constructs private pages from latest roots under the gate;
		// no old physical page or caller callback is replayed. Current constraints
		// and indexes are validated/maintained by the ordinary executor.
		return executeNativeMutationBatch(batch,expected,{}, {},0U,validate);
	}

	LogicalGraphMutationBatchResult RelocatableStorageBackend::executeNativeMutationBatch(
		const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence,
		const LogicalGraphMutationReadControl &control,
		std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
		const std::function<void(const MetadataSnapshotPin &)> &rebaseValidation) {
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

		auto workspaceGate = query::execution::detail::ProcessResourceDomains::defaultOwner()->lockWriteWorkspace(resource_domain_);
        auto writeWorkspace = mutationWriteWorkspace(resource_domain_);
        std::optional<utils::OwnedAllocationScope> writeAllocation;
        writeAllocation.emplace(writeWorkspace);

        std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		NativeGraphMutationFenceValidator fenceValidator;
		if (expectedFence.has_value()) {
			const LogicalGraphMutationFence expected = *expectedFence;
			fenceValidator = [expected](const Superblock &base) {
				return RelocatableStorageBackend::makeLogicalGraphMutationFence(base) == expected;
			};
		}

		const size_t MAX_CONFLICT_RETRIES = rebaseValidation ? 3U : 4U;
		for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
			MetadataSnapshotPin beforeExecution = metadata_->openSnapshot();
			const LogicalGraphMutationFence graphFenceBeforeExecution =
					makeLogicalGraphMutationFence(beforeExecution.superblock());
			if (rebaseValidation) {
				rebaseValidation(beforeExecution);
				fenceValidator = [graphFenceBeforeExecution](const Superblock &base) {
					return makeLogicalGraphMutationFence(base) == graphFenceBeforeExecution;
				};
			}
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
                debug::PerfTrace::addValue("native_graph.write.private_workspace_peak_bytes", static_cast<int64_t>(writeWorkspace->peakAdmitted()));
                debug::PerfTrace::addValue("native_graph.write.private_workspace_grant_bytes", static_cast<int64_t>(writeWorkspace->capacity()));
                writeAllocation.reset(); // Queue/cache publication retains its own resource class.
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
			if (expectedFence.has_value() && !rebaseValidation) {
				throw LogicalGraphMutationConflict(
						"relocatable logical graph mutation batch no longer matches its read snapshot fence");
			}
		}
		if (rebaseValidation)
			throw LogicalGraphMutationConflict("blind Property rebase exceeded its three-build limit");
		throw std::runtime_error(
				"relocatable logical graph mutation batch exceeded its optimistic conflict retry limit");
	}

#include "backend_runtime/GraphReadsAndIndexCandidates.inc"
#include "backend_runtime/VectorSearchAndTraining.inc"
#include "backend_runtime/SchemaAndAdministration.inc"

} // namespace graph::storage
