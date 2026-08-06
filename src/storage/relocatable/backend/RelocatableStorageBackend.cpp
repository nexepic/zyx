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
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageFormat.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedConstraintMutationAdapter.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/ExternalBTreeEntrySorter.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/ScalarIndexTreeReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorBaseRebuild.hpp"
#include "graph/storage/relocatable/maintenance/VectorIndexStateReclaimer.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"
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
#include "graph/vector/core/BFloat16.hpp"
#include "graph/vector/core/VectorMetric.hpp"

namespace graph::storage {

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
						&Superblock::node_entity_directory_root,
						&Superblock::edge_entity_directory_root,
						&Superblock::entity_property_directory_root,
						&Superblock::large_value_manifest_directory_root,
						&Superblock::vector_location_directory_root,
						&Superblock::vector_manifest_directory_root,
						&Superblock::state_directory_root,
		};

		[[noreturn]] void failVectorTraining(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] LogSequenceNumber nextVectorTrainingCheckpoint(const RelocatableMetadataStore &metadata) {
			MetadataSnapshotPin snapshot = metadata.openSnapshot();
			const uint64_t current = snapshot.superblock().checkpoint_lsn.value();
			snapshot.release();
			if (current == std::numeric_limits<uint64_t>::max()) {
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

		class RelocatableLogicalGraphTransaction final : public LogicalGraphTransaction {
		public:
			RelocatableLogicalGraphTransaction(RelocatableStorageBackend &backend, bool readOnly,
															   std::unique_ptr<LogicalGraphReadSnapshot> snapshot,
															   LogicalGraphMutationFence fence) :
				backend_(backend), read_only_(readOnly), snapshot_(std::move(snapshot)), fence_(std::move(fence)),
				issued_node_ids_(), issued_edge_ids_() {
				if (snapshot_ == nullptr) {
					throw std::logic_error("relocatable logical transaction could not open its read snapshot");
				}
				if (!fence_.isValid()) {
					throw std::logic_error("relocatable logical transaction requires a valid graph-state read fence");
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
					preview_reservation_.emplace();
					preview_reservation_->node_count = batch.node_creates.size();
					preview_reservation_->edge_count = batch.edge_creates.size();
					preview_reservation_->reservation = backend_.reserveLogicalGraphCreationIds(
							static_cast<uint64_t>(preview_reservation_->node_count),
							static_cast<uint64_t>(preview_reservation_->edge_count));
				} else if (preview_reservation_->node_count != batch.node_creates.size() ||
						   preview_reservation_->edge_count != batch.edge_creates.size()) {
					throw std::logic_error(
							"relocatable logical transaction must stage or discard its outstanding creation preview first");
				}
				return provisionalResult(preview_reservation_->reservation, batch.node_creates.size(), batch.edge_creates.size());
			}

			[[nodiscard]] LogicalGraphMutationBatchResult stageMutationBatch(LogicalGraphMutationBatch batch) override {
				requireActive("stage a mutation batch");
				if (read_only_) {
					throw std::logic_error("relocatable read-only logical transaction cannot stage graph mutations");
				}
				if (batch.creation_identity_plan.has_value()) {
					throw std::invalid_argument(
							"relocatable logical transaction owns creation identity reservation normalization");
				}
				const size_t nodeOffset = issued_node_ids_.size();
				const size_t edgeOffset = issued_edge_ids_.size();
				NativeGraphLogicalIdReservation reservation;
				if (preview_reservation_.has_value()) {
					if (preview_reservation_->node_count != batch.node_creates.size() ||
						preview_reservation_->edge_count != batch.edge_creates.size()) {
						throw std::logic_error(
								"relocatable logical transaction stage does not match its outstanding creation preview");
					}
					reservation = std::move(preview_reservation_->reservation);
					preview_reservation_.reset();
				} else if (!batch.node_creates.empty() || !batch.edge_creates.empty()) {
					reservation = backend_.reserveLogicalGraphCreationIds(
							static_cast<uint64_t>(batch.node_creates.size()), static_cast<uint64_t>(batch.edge_creates.size()));
				}
				const LogicalGraphMutationBatchResult result =
						provisionalResult(reservation, batch.node_creates.size(), batch.edge_creates.size());
				rebaseCreatedReferences(batch, issued_node_ids_, issued_edge_ids_);
				appendIssuedIds(reservation.node_ranges, issued_node_ids_, node_id_ranges_, "Node");
				appendIssuedIds(reservation.edge_ranges, issued_edge_ids_, edge_id_ranges_, "Edge");
				foldCreatedEntityFinalState(batch, nodeOffset, edgeOffset);
				append(accumulated_, batch);
				return result;
			}

			[[nodiscard]] LogicalGraphMutationBatchResult commit() override {
				requireActive("commit");
				if (read_only_) {
					if (!isEmpty(accumulated_)) {
						throw std::logic_error(
								"relocatable read-only logical transaction contains staged graph mutations");
					}
					state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
					snapshot_.reset();
					return {};
				}

				try {
					CanonicalMutation canonical = canonicalizeFinalState();
					LogicalGraphMutationBatchResult result =
							backend_.executeLogicalGraphMutationBatch(canonical.batch, fence_);
					if (result != canonical.durable_result) {
						throw std::logic_error(
								"relocatable logical transaction publication changed its canonical creation identity");
					}
					abandonOutstandingCreationReservations();
					state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
					snapshot_.reset();
					return result;
				} catch (const LogicalGraphMutationConflict &) {
					abandonOutstandingCreationReservations();
					state_ = LogicalGraphTransactionState::LGTTS_CONFLICTED;
					accumulated_ = {};
					snapshot_.reset();
					throw;
				} catch (...) {
					// A failed logical commit must not leave a caller with a reusable
					// transaction whose staged state may no longer be meaningful.
					abandonOutstandingCreationReservations();
					state_ = LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
					accumulated_ = {};
					snapshot_.reset();
					throw;
				}
			}

			void rollback() override {
				if (state_ != LogicalGraphTransactionState::LGTTS_ACTIVE) {
					throw std::logic_error("relocatable logical transaction is no longer active");
				}
				abandonOutstandingCreationReservations();
				accumulated_ = {};
				snapshot_.reset();
				state_ = LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
			}

		private:
			[[nodiscard]] static std::optional<size_t> findIssuedOrdinal(const std::vector<uint64_t> &ids, uint64_t id) {
				const auto iterator = std::lower_bound(ids.begin(), ids.end(), id);
				if (iterator == ids.end() || *iterator != id) {
					return std::nullopt;
				}
				return static_cast<size_t>(iterator - ids.begin());
			}

			static void rebaseReference(LogicalGraphMutationEntityReference &reference, size_t nodeOffset,
									 size_t edgeOffset, const std::vector<uint64_t> &issuedNodeIds,
									 const std::vector<uint64_t> &issuedEdgeIds) {
				if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE &&
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
				if (reference.value > std::numeric_limits<uint64_t>::max() - offset) {
					throw std::overflow_error("relocatable logical transaction batch reference ordinal overflow");
				}
				reference.value += offset;
			}

			static void rebaseCreatedReferences(LogicalGraphMutationBatch &batch, const std::vector<uint64_t> &issuedNodeIds,
												const std::vector<uint64_t> &issuedEdgeIds) {
				const size_t nodeOffset = issuedNodeIds.size();
				const size_t edgeOffset = issuedEdgeIds.size();
				if (nodeOffset > static_cast<size_t>(std::numeric_limits<uint64_t>::max()) ||
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

			[[nodiscard]] static std::vector<uint64_t> collectIssuedIds(
					std::span<const NativeGraphLogicalIdLeaseRange> ranges, size_t expectedCount, const char *entityName) {
				std::vector<uint64_t> ids;
				ids.reserve(expectedCount);
				for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
					if (range.first_id == 0U || range.count == 0U ||
						range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id ||
						range.count > expectedCount - ids.size()) {
						throw std::overflow_error(std::string("relocatable logical transaction ") + entityName +
															  " received an invalid durable ID lease");
					}
					for (uint64_t offset = 0U; offset < range.count; ++offset) {
						ids.push_back(range.first_id + offset);
					}
				}
				if (ids.size() != expectedCount || !std::ranges::is_sorted(ids) ||
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
					if (range.first_id == 0U || range.count == 0U ||
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

			static void append(LogicalGraphMutationBatch &destination, LogicalGraphMutationBatch &source) {
				destination.node_creates.insert(destination.node_creates.end(),
												std::make_move_iterator(source.node_creates.begin()),
												std::make_move_iterator(source.node_creates.end()));
				destination.edge_creates.insert(destination.edge_creates.end(),
												std::make_move_iterator(source.edge_creates.begin()),
												std::make_move_iterator(source.edge_creates.end()));
				destination.property_replacements.insert(destination.property_replacements.end(),
														 std::make_move_iterator(source.property_replacements.begin()),
														 std::make_move_iterator(source.property_replacements.end()));
				destination.node_label_replacements.insert(
						destination.node_label_replacements.end(),
						std::make_move_iterator(source.node_label_replacements.begin()),
						std::make_move_iterator(source.node_label_replacements.end()));
				destination.edge_deletes.insert(destination.edge_deletes.end(),
												std::make_move_iterator(source.edge_deletes.begin()),
												std::make_move_iterator(source.edge_deletes.end()));
				destination.node_deletes.insert(destination.node_deletes.end(),
												std::make_move_iterator(source.node_deletes.begin()),
												std::make_move_iterator(source.node_deletes.end()));
			}

			void foldCreatedEntityFinalState(LogicalGraphMutationBatch &batch, size_t nodeOffset, size_t edgeOffset) {
				const auto nodeCreate = [this, &batch,
										 nodeOffset](uint64_t ordinal) -> LogicalGraphNodeCreateRequest & {
					if (ordinal < nodeOffset) {
						return accumulated_.node_creates.at(static_cast<size_t>(ordinal));
					}
					const uint64_t local = ordinal - nodeOffset;
					if (local >= batch.node_creates.size()) {
						throw std::invalid_argument(
								"relocatable logical transaction has an invalid created Node reference");
					}
					return batch.node_creates.at(static_cast<size_t>(local));
				};
				const auto edgeCreate = [this, &batch,
										 edgeOffset](uint64_t ordinal) -> LogicalGraphBatchEdgeCreateRequest & {
					if (ordinal < edgeOffset) {
						return accumulated_.edge_creates.at(static_cast<size_t>(ordinal));
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
							persistedProperties.push_back(std::move(mutation));
							break;
					}
				}
				batch.property_replacements = std::move(persistedProperties);

				std::vector<LogicalGraphNodeLabelMutationRequest> persistedLabels;
				persistedLabels.reserve(batch.node_label_replacements.size());
				std::set<uint64_t> createdLabelTargets;
				for (LogicalGraphNodeLabelMutationRequest &mutation: batch.node_label_replacements) {
					if (mutation.node.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
						if (mutation.node.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
							!createdLabelTargets.insert(mutation.node.value).second) {
							throw std::invalid_argument(
									"relocatable logical transaction has duplicate or invalid created Node labels");
						}
						nodeCreate(mutation.node.value).labels = std::move(mutation.labels);
					} else {
						persistedLabels.push_back(std::move(mutation));
					}
				}
				batch.node_label_replacements = std::move(persistedLabels);
			}

			struct CanonicalMutation {
				LogicalGraphMutationBatch batch;
				LogicalGraphMutationBatchResult durable_result;
			};

			struct PendingCreationReservation {
				size_t node_count = 0U;
				size_t edge_count = 0U;
				NativeGraphLogicalIdReservation reservation;
			};

			[[nodiscard]] CanonicalMutation canonicalizeFinalState() const {
				const size_t nodeCount = accumulated_.node_creates.size();
				const size_t edgeCount = accumulated_.edge_creates.size();
				if (issued_node_ids_.size() != nodeCount || issued_edge_ids_.size() != edgeCount) {
					throw std::logic_error("relocatable logical transaction lost its durable creation identity");
				}
				std::vector<bool> cancelledNodes(nodeCount, false);
				std::vector<bool> cancelledEdges(edgeCount, false);
				std::vector<const LogicalGraphNodeDeleteRequest *> createdNodeDeletes(nodeCount, nullptr);
				std::set<uint64_t> uniqueNodeDeletes;
				for (const LogicalGraphNodeDeleteRequest &request: accumulated_.node_deletes) {
					if (request.node_id == 0U || !uniqueNodeDeletes.insert(request.node_id).second ||
						(request.detach && request.maximum_detach_edges == 0U)) {
						throw std::invalid_argument("relocatable logical transaction requires unique Node deletes and "
													"positive detach bounds");
					}
					const std::optional<size_t> ordinal = findIssuedOrdinal(issued_node_ids_, request.node_id);
					if (!ordinal.has_value()) {
						continue;
					}
					cancelledNodes.at(*ordinal) = true;
					createdNodeDeletes.at(*ordinal) = &request;
				}
				std::set<uint64_t> uniqueEdgeDeletes;
				for (const LogicalGraphEdgeDeleteRequest &request: accumulated_.edge_deletes) {
					if (request.edge_id == 0U || !uniqueEdgeDeletes.insert(request.edge_id).second) {
						throw std::invalid_argument(
								"relocatable logical transaction requires unique nonzero Edge deletes");
					}
					if (const std::optional<size_t> ordinal = findIssuedOrdinal(issued_edge_ids_, request.edge_id);
						ordinal.has_value()) {
						cancelledEdges.at(*ordinal) = true;
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
				for (size_t nodeOrdinal = 0U; nodeOrdinal < createdNodeDeletes.size(); ++nodeOrdinal) {
					const LogicalGraphNodeDeleteRequest *const deletion = createdNodeDeletes[nodeOrdinal];
					if (deletion == nullptr || deletion->detach) {
						continue;
					}
					for (size_t edgeOrdinal = 0U; edgeOrdinal < accumulated_.edge_creates.size(); ++edgeOrdinal) {
						const LogicalGraphBatchEdgeCreateRequest &edge = accumulated_.edge_creates[edgeOrdinal];
						const bool incident = (createdNodeOrdinal(edge.source, "Edge creation") == nodeOrdinal) ||
											  (createdNodeOrdinal(edge.target, "Edge creation") == nodeOrdinal);
						if (incident && !cancelledEdges[edgeOrdinal]) {
							throw std::invalid_argument(
									"relocatable logical transaction cannot delete a newly created Node "
									"with a live Edge without detach semantics");
						}
					}
				}
				for (size_t edgeOrdinal = 0U; edgeOrdinal < accumulated_.edge_creates.size(); ++edgeOrdinal) {
					const LogicalGraphBatchEdgeCreateRequest &edge = accumulated_.edge_creates[edgeOrdinal];
					const std::optional<size_t> source = createdNodeOrdinal(edge.source, "Edge creation");
					const std::optional<size_t> target = createdNodeOrdinal(edge.target, "Edge creation");
					if ((source.has_value() && cancelledNodes[*source]) ||
						(target.has_value() && cancelledNodes[*target])) {
						cancelledEdges[edgeOrdinal] = true;
					}
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
						case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
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
				for (const LogicalGraphPropertyMutationRequest &source: accumulated_.property_replacements) {
					LogicalGraphPropertyMutationRequest request = source;
					if ((request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
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
				for (const LogicalGraphNodeLabelMutationRequest &source: accumulated_.node_label_replacements) {
					LogicalGraphNodeLabelMutationRequest request = source;
					if (request.node.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
						request.node.value < nodeOrdinals.size() &&
						nodeOrdinals[static_cast<size_t>(request.node.value)] == std::numeric_limits<size_t>::max()) {
						continue;
					}
					remapReference(request.node, "Node label replacement");
					canonical.batch.node_label_replacements.push_back(std::move(request));
				}
				for (const LogicalGraphEdgeDeleteRequest &request: accumulated_.edge_deletes) {
					if (!findIssuedOrdinal(issued_edge_ids_, request.edge_id).has_value()) {
						canonical.batch.edge_deletes.push_back(request);
					}
				}
				for (const LogicalGraphNodeDeleteRequest &request: accumulated_.node_deletes) {
					if (!findIssuedOrdinal(issued_node_ids_, request.node_id).has_value()) {
						canonical.batch.node_deletes.push_back(request);
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
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot_;
			LogicalGraphMutationFence fence_;
			LogicalGraphMutationBatch accumulated_;
			mutable std::optional<PendingCreationReservation> preview_reservation_;
			std::vector<uint64_t> issued_node_ids_;
			std::vector<uint64_t> issued_edge_ids_;
			std::vector<LogicalGraphCreationIdentityRange> node_id_ranges_;
			std::vector<LogicalGraphCreationIdentityRange> edge_id_ranges_;
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
			if (label.empty() || propertyNames.empty()) {
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
			if (label.empty() || property.empty() || dimensions == 0U || metric.empty()) {
				throw std::invalid_argument("logical Vector Index requires a complete node schema");
			}
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
		try {
			data = OwnedRelocatableBackendFile::create(dataPath, options.buffer_pool_bytes);
			Superblock initial;
			initial.database_id = DatabaseId(generateDatabaseId());
			initial.generation = Generation(1U);
			initial.checkpoint_lsn = LogSequenceNumber(1U);
			initial.flags = 0U;
			static_cast<void>(SuperblockCodec::encode(initial));
			auto metadata = RelocatableMetadataStore::initializeNew(data->io(), initial);
			metadata.reset();
			data->io()->sync();
		} catch (...) {
			const bool removeData = static_cast<bool>(data);
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
			vector_codebooks_ = std::make_unique<VectorCodebookStore>(*physical_, *metadata_,
																	  options_.maximum_vector_codebook_cache_bytes,
																	  options_.maximum_vector_codebook_cache_entries);
			const SharedPagePoolOpenMode poolOpenMode =
					isReadOnly() ? SharedPagePoolOpenMode::SPPOM_READ_ONLY : SharedPagePoolOpenMode::SPPOM_RECONCILE;
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
			native_graph_reads_ = std::make_unique<PageBackedGraphReadAdapter>(*metadata_, *native_entities_,
																																													   *native_labels_, *properties_, *btree_pages_);
			native_logical_graph_reads_ = std::make_unique<PageBackedLogicalGraphReadAdapter>(
					*native_graph_reads_, static_cast<const PageBackedGraphNameResolver &>(*this));
			if (!isReadOnly()) {
				if (options_.maximum_index_definitions == 0U || options_.maximum_constraint_definitions == 0U ||
					options_.maximum_online_index_entries == 0U ||
					options_.maximum_online_index_entries == std::numeric_limits<size_t>::max() ||
					options_.maximum_online_index_scan_nodes == 0U ||
					options_.maximum_online_index_scan_nodes == std::numeric_limits<size_t>::max() ||
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
					options_.maximum_adjacency_read_edges == 0U ||
					options_.maximum_adjacency_read_edges == std::numeric_limits<size_t>::max() ||
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
				throw std::invalid_argument("relocatable writable backend requires finite positive index, scan, "
												"constraint, Vector, adjacency, and collision bounds");
				}
				NativeGraphWritePlannerOptions graphWriteOptions;
				graphWriteOptions.maximum_adjacency_read_edges = options_.maximum_adjacency_read_edges;
				native_graph_write_planner_ = std::make_unique<NativeGraphWritePlanner>(*metadata_, *native_entities_,
																																										*native_labels_, *properties_, *btree_pages_,
																																										graphWriteOptions);
				const NativeGraphMutationExecutorOptions nativeMutationOptions{
						options_.maximum_index_definitions,	   options_.maximum_constraint_definitions,
						options_.maximum_online_index_entries, options_.maximum_constraint_support_collisions,
						options_.maximum_online_index_entries, options_.maximum_online_index_pages,
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
			states_ = std::make_unique<StateRecordStore>(*slotted_pages_, *metadata_);
			if (options_.verify_integrity_on_open) {
				static_cast<void>(verifyIntegrity());
			}
			requires_reopen_after_indeterminate_write_.store(false, std::memory_order_release);
			if (options_.maintenance.has_value()) {
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
							const auto definition = std::ranges::find_if(
									definitions, [indexId](const IndexDefinition &candidate) { return candidate.index_id == indexId; });
							if (definition == definitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR) {
								snapshot.release();
								throw FormatException(FormatError::FERR_INVALID_STATE,
										"relocatable Vector maintenance request names an unknown Vector Index");
							}
							const std::string indexName = definition->name;
							snapshot.release();
							const LogicalGraphVectorTrainingResult rebuilt = trainLogicalVectorUnderPublicationLock(
									{indexName, options_.maximum_vector_training_records, options_.maximum_vector_training_pages});
							static_cast<void>(rebuilt);
								return RelocatableMaintenanceTaskResult{
										RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 0U, 0U};
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
											const VectorManifest manifest = vector_manifests_->read(snapshot, location.index_id).manifest();
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
							});
				// Preserve the open-time fail-closed boundary for routed Manifests. The
				// incremental recovery scanner below repeats this check for concurrent
				// schema changes, but a corrupt persisted route must not become usable
				// merely because maintenance has not received its first cycle yet.
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
					static_cast<void>(maintenance_->enqueueVectorRebuildRecovery());
					maintenance_->start();
			}
		} catch (...) {
			releaseResources();
			throw;
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

	std::unique_ptr<LogicalGraphTransaction> RelocatableStorageBackend::beginLogicalGraphTransaction(bool readOnly) {
		requireOpen();
		if (!readOnly && (isReadOnly() || native_graph_mutation_executor_ == nullptr)) {
			throw std::logic_error(
					"relocatable logical write transaction requires the native graph writer on a writable backend");
		}
		if (!readOnly && requires_reopen_after_indeterminate_write_) {
			throw std::logic_error("relocatable graph write outcome is indeterminate; close and reopen the backend "
								   "before another write");
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
			std::unique_ptr<LogicalGraphReadSnapshot> logicalSnapshot = openLogicalGraphReadSnapshot();
			if (logicalSnapshot == nullptr) {
				throw std::logic_error("relocatable logical transaction could not construct its logical read snapshot");
			}
			const std::optional<LogicalGraphMutationFence> actualFence = logicalSnapshot->mutationFence();
			if (actualFence.has_value() && *actualFence == expectedFence) {
				rawSnapshot.release();
				return std::make_unique<RelocatableLogicalGraphTransaction>(*this, readOnly, std::move(logicalSnapshot),
																																		 expectedFence);
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
		if (isReadOnly() || native_graph_write_planner_ == nullptr ||
			(nodeCount == 0U && edgeCount == 0U) || requires_reopen_after_indeterminate_write_) {
			throw std::logic_error(
					"relocatable logical creation identity reservation requires an open writable native graph backend");
		}
		return native_graph_write_planner_->reserveLogicalIds(nodeCount, edgeCount);
	}

	void RelocatableStorageBackend::abandonLogicalGraphCreationIds(
			const NativeGraphLogicalIdReservation &reservation) noexcept {
		if (metadata_ != nullptr) {
			metadata_->abandonNativeGraphLogicalIdReservation(reservation);
		}
	}

	std::vector<uint64_t>
	RelocatableStorageBackend::createLogicalNodes(const std::vector<LogicalGraphNodeCreateRequest> &requests) {
		requireOpen();
		if (requests.empty()) {
			return {};
		}
		if (isReadOnly() || native_graph_mutation_executor_ == nullptr) {
			throw std::logic_error("relocatable logical Node creation requires the native graph writer on a writable backend");
		}
		if (requires_reopen_after_indeterminate_write_) {
			throw std::logic_error("relocatable graph write outcome is indeterminate; close and reopen the backend "
								   "before another write");
		}
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
		if (isReadOnly() || native_graph_mutation_executor_ == nullptr) {
			throw std::logic_error("relocatable logical Edge creation requires the native graph writer on a writable backend");
		}
		if (requires_reopen_after_indeterminate_write_) {
			throw std::logic_error("relocatable graph write outcome is indeterminate; close and reopen the backend "
								   "before another write");
		}
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
		if (isReadOnly() || native_graph_mutation_executor_ == nullptr) {
			throw std::logic_error(
					"relocatable logical Node Property replacement requires the native graph writer on a writable backend");
		}
		if (requires_reopen_after_indeterminate_write_) {
			throw std::logic_error("relocatable graph write outcome is indeterminate; close and reopen the backend "
								   "before another write");
		}
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
		if (isReadOnly() || native_graph_mutation_executor_ == nullptr) {
			throw std::logic_error(
					"relocatable logical Edge Property replacement requires the native graph writer on a writable backend");
		}
		if (requires_reopen_after_indeterminate_write_) {
			throw std::logic_error("relocatable graph write outcome is indeterminate; close and reopen the backend "
								   "before another write");
		}
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
		if (isReadOnly() || native_graph_mutation_executor_ == nullptr) {
			throw std::logic_error("relocatable logical Edge deletion requires the native graph writer on a writable backend");
		}
		if (requires_reopen_after_indeterminate_write_) {
			throw std::logic_error("relocatable graph write outcome is indeterminate; close and reopen the backend "
								   "before another write");
		}
		LogicalGraphMutationBatch batch;
		batch.edge_deletes = requests;
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void RelocatableStorageBackend::deleteLogicalNodes(const std::vector<LogicalGraphNodeDeleteRequest> &requests) {
		requireOpen();
		if (requests.empty()) {
			return;
		}
		if (isReadOnly() || native_graph_mutation_executor_ == nullptr) {
			throw std::logic_error("relocatable logical Node deletion requires the native graph writer on a writable backend");
		}
		if (requires_reopen_after_indeterminate_write_) {
			throw std::logic_error("relocatable graph write outcome is indeterminate; close and reopen the backend "
								   "before another write");
		}
		LogicalGraphMutationBatch batch;
		batch.node_deletes = requests;
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	LogicalGraphMutationBatchResult RelocatableStorageBackend::executeLogicalGraphMutationBatch(
			const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence) {
		requireOpen();
		if (batch.node_creates.empty() && batch.edge_creates.empty() && batch.property_replacements.empty() &&
			batch.node_label_replacements.empty() && batch.edge_deletes.empty() && batch.node_deletes.empty() &&
			!batch.creation_identity_plan.has_value()) {
			return {};
		}
		if (isReadOnly() || native_graph_mutation_executor_ == nullptr) {
			throw std::logic_error(
					"relocatable logical graph mutation batches require the native graph writer on a writable backend");
		}
		if (requires_reopen_after_indeterminate_write_) {
			throw std::logic_error("relocatable graph write outcome is indeterminate; close and reopen the backend "
								   "before another write");
		}
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
			const NativeGraphMutationExecutionResult execution =
					native_graph_mutation_executor_->execute(batch, fenceValidator);
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
		if (native_logical_graph_reads_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Node read requires the native logical graph reader");
		}
		return native_logical_graph_reads_->readNode(snapshot.pin_, nodeId);
	}

	LogicalGraphEdge RelocatableStorageBackend::readEdge(const RelocatableReadSnapshot &snapshot,
																			  uint64_t edgeId) const {
		requireOwnedSnapshot(snapshot);
		if (native_logical_graph_reads_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Edge read requires the native logical graph reader");
		}
		return native_logical_graph_reads_->readEdge(snapshot.pin_, edgeId);
	}

	bool RelocatableStorageBackend::containsLogicalNode(const RelocatableReadSnapshot &snapshot,
																			uint64_t nodeId) const {
		requireOwnedSnapshot(snapshot);
		if (nodeId == 0U) {
			return false;
		}
		return metadata_->findNodePageLocationOptional(snapshot.pin_, nodeId).has_value();
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNativeNodesByExactProperties(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
			size_t maximumPropertyEntries) const {
		if (!snapshot.isValid() || maximumNodes == 0U || maximumPropertyEntries == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable native exact Node lookup requires an active snapshot and positive bounds");
		}
		if (native_graph_reads_ == nullptr || native_logical_graph_reads_ == nullptr || system_catalog_ == nullptr ||
			btree_pages_ == nullptr || metadata_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native exact Node lookup requires native graph, Catalog, and Index stores");
		}
		if (labels.size() != 1U || labels.front().empty() || properties.empty()) {
			return std::nullopt;
		}
		std::map<std::string, const PropertyValue *> predicateValues;
		for (const auto &[name, value]: properties) {
			if (name.empty() || value.getType() == PropertyType::NULL_TYPE ||
				!predicateValues.emplace(name, &value).second) {
				return std::nullopt;
			}
		}
		const std::optional<uint64_t> labelToken = system_catalog_->findSchemaToken(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, labels.front());
		if (!labelToken.has_value()) {
			return std::vector<LogicalGraphNode>{};
		}
		if (options_.maximum_constraint_support_collisions == 0U ||
			options_.maximum_constraint_support_collisions == std::numeric_limits<size_t>::max()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable native exact Node lookup requires a finite support-tree collision bound");
		}
		const std::vector<ConstraintDefinition> definitions =
				readAllConstraintDefinitions(snapshot, options_.maximum_constraint_definitions);
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
				const std::optional<uint32_t> token = system_catalog_->findPropertyToken(snapshot, name);
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
								 options_.maximum_constraint_support_collisions + 1U);
			size_t collisionCount = 0U;
			std::vector<LogicalGraphNode> result;
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
				const PageBackedGraphNode node = native_graph_reads_->readNode(snapshot, nodeId);
				if (node.node_id != nodeId ||
					std::ranges::find(node.label_token_ids, definition.schema_token_id) == node.label_token_ids.end() ||
					node.properties.size() > maximumPropertyEntries) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup found an invalid constrained Node");
				}
				std::vector<PropertyValue> actualValues;
				actualValues.reserve(propertyTokens.size());
				for (const uint32_t token: propertyTokens) {
					const auto property = std::ranges::lower_bound(node.properties, token, {},
																		&PropertyCollectionRootEntry::key_token);
					if (property == node.properties.end() || property->key_token != token ||
						property->value.getType() == PropertyType::NULL_TYPE) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable native exact Node lookup found a stale support-tree entry");
					}
					actualValues.push_back(property->value);
				}
				// A matching digest is only a routing candidate: retain the canonical
				// value comparison so a SHA-256 collision cannot become a false match.
				if (ConstraintSupportTree::canonicalKey(actualValues) != requestedKey) {
					continue;
				}
				if (result.size() == maximumNodes) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup exceeded its caller-supplied result bound");
				}
				result.push_back(native_logical_graph_reads_->readNode(snapshot, nodeId));
			}
			if (result.size() > 1U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable native exact Node lookup found duplicate unique-constraint entries");
			}
			return result;
		}
		return std::nullopt;
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNodesByExactProperties(
			const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
			size_t maximumPropertyEntries) const {
		requireOwnedSnapshot(snapshot);
		return findNativeNodesByExactProperties(snapshot.pin_, labels, properties, maximumNodes, maximumPropertyEntries);
	}

	void RelocatableStorageBackend::visitNodes(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
															   const std::function<void(const LogicalGraphNode &)> &visitor) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Node scan requires a visitor");
		}
		static_cast<void>(visitNodesUntil(snapshot, maximumRecords, [&visitor](const LogicalGraphNode &node) {
					visitor(node);
					return true;
				}));
	}

	void RelocatableStorageBackend::visitEdges(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
															   const std::function<void(const LogicalGraphEdge &)> &visitor) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Edge scan requires a visitor");
		}
		static_cast<void>(visitEdgesUntil(snapshot, maximumRecords, [&visitor](const LogicalGraphEdge &edge) {
					visitor(edge);
					return true;
				}));
	}

	bool RelocatableStorageBackend::visitNodesUntil(
			const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
			const std::function<bool(const LogicalGraphNode &)> &visitor) const {
		requireOwnedSnapshot(snapshot);
		if (maximumRecords == 0U || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Node scan requires a positive bound and visitor");
		}
		if (native_logical_graph_reads_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Node scan requires the native logical graph reader");
		}
		return native_logical_graph_reads_->visitNodesUntil(snapshot.pin_, maximumRecords, visitor);
	}

	bool RelocatableStorageBackend::visitEdgesUntil(
			const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
			const std::function<bool(const LogicalGraphEdge &)> &visitor) const {
		requireOwnedSnapshot(snapshot);
		if (maximumRecords == 0U || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Edge scan requires a positive bound and visitor");
		}
		if (native_logical_graph_reads_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Edge scan requires the native logical graph reader");
		}
		return native_logical_graph_reads_->visitEdgesUntil(snapshot.pin_, maximumRecords, visitor);
	}

	LogicalGraphNodePage RelocatableStorageBackend::scanNodesAfter(
			const RelocatableReadSnapshot &snapshot, uint64_t afterNodeId, size_t maximumRecords,
			size_t maximumMetadataPages) const {
		requireOwnedSnapshot(snapshot);
		if (maximumRecords == 0U || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable resumable Node scan requires positive bounds");
		}
		if (native_logical_graph_reads_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable resumable Node scan requires the native logical graph reader");
		}
		return native_logical_graph_reads_->scanNodesAfter(snapshot.pin_, afterNodeId, maximumRecords,
																					maximumMetadataPages);
	}

	LogicalGraphAdjacencyPage RelocatableStorageBackend::scanAdjacentEdges(
			const RelocatableReadSnapshot &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const {
		requireOwnedSnapshot(snapshot);
		if (native_logical_graph_reads_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable resumable adjacency scan requires the native logical graph reader");
		}
		return native_logical_graph_reads_->scanAdjacentEdges(snapshot.pin_, nodeId, direction, std::move(continuation),
																												maximumRecords);
	}

	void RelocatableStorageBackend::visitAdjacentEdges(
			const RelocatableReadSnapshot &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			size_t maximumEdges, const std::function<void(const LogicalGraphEdge &)> &visitor) const {
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
		if (native_logical_graph_reads_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable adjacency traversal requires the native logical graph reader");
		}
		return native_logical_graph_reads_->visitAdjacentEdgesUntil(snapshot.pin_, nodeId, direction, maximumEdges,
																								 visitor);
	}

	std::optional<uint32_t> RelocatableStorageBackend::findPropertyKeyToken(const RelocatableReadSnapshot &snapshot,
																															std::string_view key) const {
		requireOwnedSnapshot(snapshot);
		if (system_catalog_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Property-key lookup requires the native System Catalog");
		}
		return system_catalog_->findPropertyToken(snapshot.pin_, key);
	}

	std::optional<std::string> RelocatableStorageBackend::findPropertyKey(const RelocatableReadSnapshot &snapshot,
																													  uint32_t token) const {
		requireOwnedSnapshot(snapshot);
		if (system_catalog_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Property-key lookup requires the native System Catalog");
		}
		return system_catalog_->findPropertyName(snapshot.pin_, token);
	}

	std::optional<TokenDefinition> RelocatableStorageBackend::findSchemaToken(const RelocatableReadSnapshot &snapshot,
																																		  uint64_t tokenId) const {
		requireOwnedSnapshot(snapshot);
		if (system_catalog_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable schema Token lookup requires the native System Catalog");
		}
		const std::optional<SystemCatalogSchemaToken> label = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_LABEL, tokenId);
		const std::optional<SystemCatalogSchemaToken> relationshipType = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, tokenId);
		if (label.has_value() && relationshipType.has_value()) {
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
		if (system_catalog_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable schema Token lookup requires the native System Catalog");
		}
		const std::optional<uint64_t> label = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
		const std::optional<uint64_t> relationshipType = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, name);
		if (label.has_value() && relationshipType.has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable schema Token name is ambiguous across native schema domains");
		}
		return label.has_value() ? label : relationshipType;
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
			request.maximum_mapping_entries == std::numeric_limits<size_t>::max()) {
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
		if (vector_manifests_ == nullptr || vector_pages_ == nullptr || vector_codebooks_ == nullptr ||
			btree_pages_ == nullptr || metadata_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical Vector search has an incomplete persistent Vector state");
		}

		// An index may validly precede its first matching Property. In that state no
		// physical mapping or Manifest exists yet, so the exact answer is empty.
		if (!metadata_->findVectorManifestOptional(snapshot.pin_, definition->index_id).has_value()) {
			return {};
		}
		const VectorManifest manifest = vector_manifests_->read(snapshot.pin_, definition->index_id).manifest();
		VectorAdjacencyPageStore adjacencies(data_file_->io(), *metadata_);
		return RelocatableVectorSearch::search(snapshot.pin_, *definition, manifest, request, *btree_pages_,
											   *vector_pages_, adjacencies, *vector_codebooks_, *metadata_);
	}

	LogicalGraphVectorTrainingResult
	RelocatableStorageBackend::trainLogicalVector(const LogicalGraphVectorTrainingRequest &request) {
		requireOpen();
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		return trainLogicalVectorUnderPublicationLock(request);
	}

	LogicalGraphVectorTrainingResult
	RelocatableStorageBackend::trainLogicalVectorUnderPublicationLock(const LogicalGraphVectorTrainingRequest &request) {
		requireOpen();
		if (isReadOnly() || native_graph_write_planner_ == nullptr) {
			throw std::runtime_error("relocatable read-only backend cannot train a logical Vector Index");
		}
		if (request.index_name.empty() || request.maximum_records == 0U || request.maximum_pages == 0U ||
			request.maximum_records == std::numeric_limits<size_t>::max() ||
			options_.maximum_vector_training_records == 0U || options_.maximum_vector_training_pages == 0U ||
			options_.maximum_vector_training_samples == 0U || options_.vector_training_iterations == 0U) {
			throw std::invalid_argument("relocatable logical Vector training requires a nonempty index name and finite "
										"positive resource bounds");
		}
		if (metadata_ == nullptr || btree_pages_ == nullptr || vector_pages_ == nullptr ||
			vector_codebooks_ == nullptr || vector_manifests_ == nullptr) {
			failVectorTraining(FormatError::FERR_INVALID_STATE,
							   "relocatable logical Vector training has an incomplete persistent Vector state");
		}

		const size_t maximumRecords = std::min(request.maximum_records, options_.maximum_vector_training_records);
		const size_t maximumPages = std::min(request.maximum_pages, options_.maximum_vector_training_pages);
		if (maximumRecords == 0U || maximumPages == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument("relocatable logical Vector training has an invalid effective resource bound");
		}

		LogicalGraphVectorTrainingResult result;
		{
			MetadataSnapshotPin sourceSnapshot = metadata_->openSnapshot();
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
							.scan(sourceSnapshot, std::nullopt, maximumRecords + 1U);
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
							.scan(sourceSnapshot, std::nullopt, maximumRecords + 1U);
			if (reverse.size() != semanticPairs.size()) {
				failVectorTraining(
						FormatError::FERR_INVALID_STATE,
						"relocatable logical Vector training found mismatched semantic mapping cardinalities");
			}
			for (size_t ordinal = 0U; ordinal < reverse.size(); ++ordinal) {
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
			rebuild.pq.pages = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()), Generation(1U),
								maximumRecords, maximumPqManifestPages};
			rebuild.pq.mapping.maximum_entries = maximumRecords;
			rebuild.pq.mapping.tree = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()), 0U,
									maximumRecords, maximumPages};

			const bool hasAnyAdjacencyIdentity = manifest.adjacency_mapping_index_id != 0U ||
					manifest.adjacency_mapping_root_page_id.isValid() || manifest.adjacency_mapping_root_generation.isValid();
			const bool hasCompleteAdjacencyIdentity =
					VectorAdjacencyMapping::isMappingIndexId(manifest.adjacency_mapping_index_id) &&
					manifest.adjacency_mapping_root_page_id.isValid() && manifest.adjacency_mapping_root_generation.isValid() &&
					manifest.adjacency_generation.isValid() && manifest.entry_point_vector_id != 0U;
			if ((hasAnyPqIdentity(manifest) && !hasCompletePqIdentity(manifest)) ||
				(hasAnyAdjacencyIdentity && !hasCompleteAdjacencyIdentity)) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found a partial derived Vector generation");
			}

			const bool hasDelta = manifest.delta_mapping_index_id != 0U ||
					manifest.delta_mapping_root_page_id.isValid() || manifest.delta_mapping_root_generation.isValid() ||
					manifest.delta_entry_count != 0U;
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
			bool reuseVisibleCodebook = false;
			if (!hasDelta && hasCompletePqIdentity(manifest) &&
				VectorCodebookExtentIdCodec::canEncodeCodebookId(manifest.codebook_id)) {
				const VectorCodebookManifest existingCodebook =
						vector_codebooks_->manifest(sourceSnapshot, manifest.codebook_id);
				const VectorCodebookHeader &header = existingCodebook.header;
				if (header.generation == manifest.codebook_generation &&
					header.element_type == rebuild.pq.codebook.element_type && header.dimensions == training.dimensions &&
					header.subspace_count == training.subspace_count &&
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
				rebuild.adjacency.adjacency_generation = hasDelta
												 ? Generation(manifest.adjacency_generation.value() + 1U)
												 : manifest.adjacency_generation;
			} else {
				rebuild.adjacency.adjacency_generation = Generation(1U);
			}

			const bool needsPqMappingId = rebuild.pq.mapping_index_id == 0U;
			const bool needsAdjacencyMappingId = rebuild.adjacency.mapping_index_id == 0U;
			if (!reuseVisibleCodebook) {
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
				const size_t mappingIdentityCount = static_cast<size_t>(needsPqMappingId) +
													static_cast<size_t>(needsAdjacencyMappingId);
				const std::vector<PageId> pageIds = metadata_->allocateLogicalPageIds(
						mappingIdentityCount + 1U, nextVectorTrainingCheckpoint(*metadata_));
				const PageId codebookAllocatorId = pageIds.front();
				rebuild.pq.codebook.codebook_id = codebookAllocatorId.value();
				rebuild.pq.codebook.manifest_segment_id = segmentIds.front();
				rebuild.pq.codebook.generation = Generation(1U);
				rebuild.pq.codebook.data_extents.reserve(dataExtentCount);
				for (size_t ordinal = 0U; ordinal < dataExtentCount; ++ordinal) {
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
					rebuild.pq.mapping_index_id = derivedVectorIdentity(
							RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal++));
					rebuild.pq.mapping.tree.pool_owner_id = rebuild.pq.mapping_index_id;
				}
				if (needsAdjacencyMappingId) {
					rebuild.adjacency.mapping_index_id = derivedVectorIdentity(
							RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal));
					rebuild.adjacency.mapping.tree.pool_owner_id = rebuild.adjacency.mapping_index_id;
				}
			} else if (needsPqMappingId || needsAdjacencyMappingId) {
				const size_t mappingCount = static_cast<size_t>(needsPqMappingId) +
									 static_cast<size_t>(needsAdjacencyMappingId);
				const std::vector<PageId> pageIds =
						metadata_->allocateLogicalPageIds(mappingCount, nextVectorTrainingCheckpoint(*metadata_));
				size_t mappingOrdinal = 0U;
				if (needsPqMappingId) {
					rebuild.pq.mapping_index_id = derivedVectorIdentity(
							RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal++));
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

			VectorAdjacencyPageStore adjacencyPages(data_file_->io(), *metadata_);
			const OfflineVectorBaseRebuildResult published = OfflineVectorBaseRebuild::run(
					*btree_pages_, *vector_pages_, adjacencyPages, *vector_codebooks_, *vector_manifests_, *metadata_,
					definition->index_id, records, rebuild);
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
		if (system_catalog_ != nullptr) {
			const std::vector<IndexDefinition> canonical =
					system_catalog_->readIndexDefinitions(snapshot, maximumDefinitions);
			for (const IndexDefinition &definition: canonical) {
				if (definition.kind != IndexDefinitionKind::IDK_VECTOR &&
						!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable canonical Index definition has no published physical root");
				}
			}
			// An initialized canonical catalog is authoritative even when the
			// current definition set is empty. Falling back here would resurrect
			// a dropped definition from an independent legacy registry.
			return canonical;
		}
		throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable backend has no canonical System Catalog");
	}

	std::vector<ConstraintDefinition>
	RelocatableStorageBackend::readAllConstraintDefinitions(const MetadataSnapshotPin &snapshot,
																		 size_t maximumDefinitions) const {
		if (maximumDefinitions == 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
																		 "relocatable constraint definition read requires a positive bound and initialized catalog");
		}
		if (system_catalog_ != nullptr) {
			return system_catalog_->readConstraintDefinitions(snapshot, maximumDefinitions);
		}
		throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable backend has no canonical System Catalog");
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

	LogicalGraphIndexDefinition
	RelocatableStorageBackend::createLogicalNodeIndex(const LogicalGraphNodeIndexCreateRequest &request) {
		const std::vector<LogicalGraphIndexDefinition> created =
				createLogicalNodeIndexes(std::span<const LogicalGraphNodeIndexCreateRequest>(&request, 1U));
		if (created.size() != 1U) {
			throw std::logic_error("relocatable logical node Index creation did not return exactly one definition");
		}
		return created.front();
	}

	std::vector<LogicalGraphIndexDefinition> RelocatableStorageBackend::createLogicalNodeIndexes(
			const std::span<const LogicalGraphNodeIndexCreateRequest> requests) {
		requireOpen();
		if (isReadOnly() || native_graph_write_planner_ == nullptr || native_graph_reads_ == nullptr ||
			btree_pages_ == nullptr || system_catalog_ == nullptr) {
			throw std::runtime_error("relocatable read-only backend cannot create a logical Index");
		}
		if (requests.empty() || requests.size() > options_.maximum_index_definitions ||
			options_.maximum_online_index_entries == 0U || options_.maximum_online_index_scan_nodes == 0U ||
			options_.maximum_online_index_pages == 0U ||
			options_.online_index_sort_run_bytes == 0U || options_.maximum_online_index_sort_runs == 0U) {
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
			definition.name = request.name.empty() ? defaultNodeIndexName(request.label, request.property_keys) : request.name;
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
			if (std::ranges::any_of(existing, [&](const IndexDefinition &candidate) {
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
			result.push_back({definition.index_id, definition.name, logicalIndexKind(definition.kind), request.label,
							  definition.property_names, 0U, {}});
			definitions.push_back(std::move(definition));
			propertyTokens.push_back(std::move(tokens));
		}

		for (size_t definitionIndex = 0U; definitionIndex < definitions.size(); ++definitionIndex) {
			const IndexDefinition &definition = definitions[definitionIndex];
			const std::vector<uint32_t> &tokens = propertyTokens[definitionIndex];
			ExternalBTreeEntrySorter sorter({options_.maximum_online_index_entries,
																			 options_.online_index_sort_run_bytes,
																			 options_.maximum_online_index_sort_runs});
			const bool scannedAllNodes = native_graph_reads_->visitNodesUntil(
					snapshot, options_.maximum_online_index_scan_nodes, [&](const PageBackedGraphNode &node) {
						if (!std::ranges::binary_search(node.label_token_ids, definition.label_token_id)) {
							return true;
						}
						std::vector<graph::PropertyValue> values;
						values.reserve(tokens.size());
						for (const uint32_t token: tokens) {
							const auto property = std::ranges::lower_bound(node.properties, token, {},
																															 &PropertyCollectionRootEntry::key_token);
							if (property == node.properties.end() || property->key_token != token) {
								return true;
							}
							values.push_back(property->value);
						}
						const std::optional<std::vector<std::byte>> key =
								definition.kind == IndexDefinitionKind::IDK_PROPERTY
										? IndexKeyCodec::tryEncodeEntryKey(values.front(), node.node_id)
										: IndexKeyCodec::tryEncodeCompositeEntryKey(values, node.node_id);
						if (key.has_value()) {
							sorter.add({std::move(*key), node.node_id});
						}
						return true;
					});
			if (!scannedAllNodes) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable logical node Index graph scan exceeded its configured bounded DDL work");
			}
			const OrderedBTreeEntrySource source = sorter.finish();
			const BulkBTreeBuildOptions build = {PhysicalSlotId(1U),
																						 PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
																						 definition.index_id,
																						 options_.maximum_online_index_entries,
																						 options_.maximum_online_index_pages};
			transaction.stageScalarIndexCreate(
					*btree_pages_, BulkBTreeBuilder::planNativeTree(*btree_pages_, *metadata_, snapshot,
																						 definition.index_id, source, build));
		}
		catalogSession.finish();
		const NativeGraphWriteCommitResult committed = transaction.commit();
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical node Index batch lost its native publication fence");
		}
		return result;
	}

	LogicalGraphIndexDefinition
	RelocatableStorageBackend::createLogicalNodeVectorIndex(const LogicalGraphNodeVectorIndexCreateRequest &request) {
		requireOpen();
		if (isReadOnly() || native_graph_write_planner_ == nullptr || native_graph_reads_ == nullptr ||
			btree_pages_ == nullptr || vector_pages_ == nullptr || vector_manifests_ == nullptr ||
			system_catalog_ == nullptr) {
			throw std::runtime_error("relocatable read-only backend cannot create a logical Vector Index");
		}
		if (requires_reopen_after_indeterminate_write_) {
			throw std::logic_error("relocatable graph write outcome is indeterminate; close and reopen the backend "
								   "before another write");
		}
		if (request.label.empty() || request.property_key.empty() ||
			request.property_key.size() > RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES || request.dimensions == 0U ||
			request.dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS || options_.maximum_online_index_entries == 0U ||
			options_.maximum_online_index_scan_nodes == 0U ||
			options_.maximum_online_index_pages == 0U) {
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
		const bool scannedAllNodes = native_graph_reads_->visitNodesUntil(
				snapshot, options_.maximum_online_index_scan_nodes, [&](const PageBackedGraphNode &node) {
					if (!std::ranges::binary_search(node.label_token_ids, definition.label_token_id)) {
						return true;
					}
					const auto property = std::ranges::lower_bound(node.properties, propertyToken, {},
																   &PropertyCollectionRootEntry::key_token);
					if (property == node.properties.end() || property->key_token != propertyToken) {
						return true;
					}
					if (records.size() == options_.maximum_online_index_entries) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable logical Vector Index exceeds its configured historical Vector bound");
					}
					const std::vector<std::byte> encoded = VectorSemanticCodec::encodeBFloat16ForLogicalWrite(
							property->value, definition.vector_dimensions);
					if (distanceMetric == VectorDistanceMetric::VDM_COSINE &&
						!VectorSemanticCodec::hasNonZeroFiniteL2Norm(encoded)) {
						throw std::invalid_argument(
								"relocatable logical cosine Vector Index cannot include a zero-norm property");
					}
					records.push_back({node.node_id, encoded});
					return true;
				});
		if (!scannedAllNodes) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable logical Vector Index graph scan exceeded its configured bounded DDL work");
		}
		if (!records.empty()) {
			NativeGraphVectorBootstrapOptions bootstrap;
			bootstrap.first_slot = PhysicalSlotId(1U);
			bootstrap.last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
			bootstrap.maximum_vectors = options_.maximum_online_index_entries;
			bootstrap.maximum_pages = options_.maximum_online_index_pages;
			transaction.stageVectorIndexBootstrap(*btree_pages_, *vector_pages_, *vector_manifests_, definition,
												  records, bootstrap);
		}
		catalogSession.finish();
		const NativeGraphWriteCommitResult committed = transaction.commit();
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
		if (isReadOnly() || native_graph_write_planner_ == nullptr || system_catalog_ == nullptr ||
			btree_pages_ == nullptr) {
			throw std::runtime_error("relocatable read-only backend cannot drop a logical Index");
		}
		if (name.empty()) {
			throw std::invalid_argument("relocatable logical Index drop requires a nonempty name");
		}
		bool dropped = false;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			const std::optional<IndexDefinition> canonical =
					system_catalog_->findIndexDefinitionByName(snapshot, name);
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
				const NativeGraphWriteCommitResult committed = transaction.commit();
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
		if (system_catalog_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable logical node Index drop requires the canonical System Catalog");
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
		if (system_catalog_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable logical Index list requires the canonical System Catalog");
		}
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::vector<IndexDefinition> definitions = readAllIndexDefinitions(snapshot, maximumIndexes);
		std::vector<LogicalGraphIndexDefinition> result;
		result.reserve(definitions.size());
		for (const IndexDefinition &definition: definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
				definition.kind == IndexDefinitionKind::IDK_LABEL) {
				continue;
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
			result.push_back({definition.index_id, definition.name, logicalIndexKind(definition.kind),
							  label->name,
							  definition.property_names, definition.vector_dimensions, definition.vector_metric});
		}
		return result;
	}

	LogicalGraphConstraintDefinition
	RelocatableStorageBackend::createLogicalConstraint(const LogicalGraphConstraintCreateRequest &request) {
		requireOpen();
		if (isReadOnly() || native_graph_write_planner_ == nullptr || native_graph_reads_ == nullptr ||
			btree_pages_ == nullptr || system_catalog_ == nullptr) {
			throw std::runtime_error("relocatable read-only backend cannot create a logical constraint");
		}
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
				});
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
		catalogSession.finish();
		const NativeGraphWriteCommitResult committed = transaction.commit();
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical constraint lost its native publication fence");
		}
		return logicalConstraintDefinition(definition, request.label);
	}

	bool RelocatableStorageBackend::dropLogicalConstraintByName(std::string_view name) {
		requireOpen();
		if (isReadOnly() || native_graph_write_planner_ == nullptr || btree_pages_ == nullptr ||
			system_catalog_ == nullptr) {
			throw std::runtime_error("relocatable read-only backend cannot drop a logical constraint");
		}
		if (name.empty()) {
			throw std::invalid_argument("relocatable logical constraint drop requires a nonempty name");
		}
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		const std::optional<ConstraintDefinition> canonical =
				system_catalog_->findConstraintDefinitionByName(transaction.baseSnapshot(), name);
		if (canonical.has_value()) {
			SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
			NativeConstraintDefinitionWriter::stageDrop(transaction, *system_catalog_, catalogSession, *btree_pages_,
																														 *metadata_, *canonical, options_.maximum_online_index_pages);
			catalogSession.finish();
			const NativeGraphWriteCommitResult committed = transaction.commit();
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
			if (system_catalog_ == nullptr) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable logical constraint list requires the native System Catalog");
			}
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
		if (isReadOnly() || btree_pages_ == nullptr || system_catalog_ == nullptr || vector_manifests_ == nullptr ||
			metadata_ == nullptr) {
			throw std::runtime_error("relocatable read-only backend cannot reclaim obsolete scalar Index trees");
		}
		ScalarIndexTreeReclaimReport report;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable scalar Index-tree reclamation cannot advance the checkpoint LSN");
			}
			const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			ScalarIndexTreeReachabilityReclaimer reclaimer(*btree_pages_, *system_catalog_, *vector_manifests_, *metadata_);
			report = reclaimer.reclaim(options, checkpointLsn, publishOptions);
		}
		return report;
	}

	VectorIndexStateReclaimReport
	RelocatableStorageBackend::reclaimObsoleteVectorIndexState(const VectorIndexStateReclaimOptions &options,
															   const MetadataPublishOptions &publishOptions) {
		requireOpen();
		if (isReadOnly() || physical_ == nullptr || slotted_pages_ == nullptr || btree_pages_ == nullptr ||
			vector_pages_ == nullptr || vector_manifests_ == nullptr || system_catalog_ == nullptr || metadata_ == nullptr ||
			data_file_ == nullptr) {
			throw std::runtime_error("relocatable read-only backend cannot reclaim obsolete Vector Index state");
		}
		VectorIndexStateReclaimReport report;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable Vector Index reclamation cannot advance the checkpoint LSN");
			}
			const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			VectorAdjacencyPageStore adjacency(data_file_->io(), *metadata_);
			VectorIndexStateReclaimer reclaimer(*physical_, *slotted_pages_, *btree_pages_, *vector_pages_, adjacency,
												*vector_manifests_, *system_catalog_, *metadata_);
			report = reclaimer.reclaim(options, checkpointLsn, publishOptions);
		}
		return report;
	}

	void RelocatableStorageBackend::pauseMaintenance() { requireMaintenance().pause(); }

	void RelocatableStorageBackend::resumeMaintenance() { requireMaintenance().resume(); }

	size_t RelocatableStorageBackend::cancelQueuedMaintenance() { return requireMaintenance().cancelQueuedTasks(); }

	uint64_t RelocatableStorageBackend::enqueuePropertyPageCompaction(SegmentId source, SegmentId destination,
																	  uint32_t pageBudget) {
		return requireMaintenance().enqueuePropertyPageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueBTreePageCompaction(SegmentId source, SegmentId destination,
																   uint32_t pageBudget) {
		return requireMaintenance().enqueueBTreePageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueVectorPageCompaction(SegmentId source, SegmentId destination,
																	uint32_t pageBudget) {
		return requireMaintenance().enqueueVectorPageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueVectorManifestCompaction(SegmentId source, SegmentId destination,
																		uint32_t pageBudget) {
		return requireMaintenance().enqueueVectorManifestCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueEntityPageCompaction(SegmentId source, SegmentId destination,
																		uint32_t pageBudget) {
		return requireMaintenance().enqueueEntityPageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueLargeValuePageCompaction(SegmentId source, SegmentId destination,
																		  uint32_t pageBudget) {
		return requireMaintenance().enqueueLargeValuePageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueCodebookExtentRelocation(uint64_t codebookId, ExtentId extentId,
																		PhysicalSlotId firstDestination,
																		PhysicalSlotId lastDestination) {
		return requireMaintenance().enqueueCodebookExtentRelocation(codebookId, extentId, firstDestination,
																	lastDestination);
	}

	RelocatableMaintenanceMetrics RelocatableStorageBackend::maintenanceMetrics() const {
		return requireMaintenance().metrics();
	}

	std::exception_ptr RelocatableStorageBackend::maintenanceFailure() const {
		return requireMaintenance().lastFailure();
	}

	StateRecord RelocatableStorageBackend::readState(const RelocatableReadSnapshot &snapshot, uint64_t stateId) const {
		requireOwnedSnapshot(snapshot);
		return states_->read(snapshot.pin_, stateId);
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
			options_.maximum_online_index_entries == std::numeric_limits<size_t>::max() ||
			native_graph_reads_ == nullptr || system_catalog_ == nullptr || btree_pages_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable native constraint support-tree integrity verification requires local "
								  "stores and finite bounds");
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
			const size_t scanBound = options_.maximum_online_index_entries + 1U;
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
							.scan(snapshot, std::nullopt, options_.maximum_online_index_entries + 1U);
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
		if (tokenId == 0U || system_catalog_ == nullptr) {
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
		if (tokenId == 0U || system_catalog_ == nullptr) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid relationship-type resolver state");
		}
		const std::optional<SystemCatalogSchemaToken> token = system_catalog_->findSchemaToken(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, tokenId);
		if (!token.has_value() || token->name.empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown relationship-type Token");
		}
		return token->name;
	}

	std::string RelocatableStorageBackend::resolvePropertyKey(const MetadataSnapshotPin &snapshot,
															  uint32_t tokenId) const {
		if (tokenId == 0U || system_catalog_ == nullptr) {
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

	void RelocatableStorageBackend::releaseResources() noexcept {
		if (maintenance_ != nullptr) {
			try {
				maintenance_->stop();
			} catch (...) {
				std::terminate();
			}
		}
		maintenance_.reset();
		states_.reset();
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
