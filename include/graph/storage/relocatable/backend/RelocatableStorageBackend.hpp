/**
 * @file RelocatableStorageBackend.hpp
 * @brief Format-safe logical read backend for a complete relocatable database.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphAdministration.hpp"
#include "graph/storage/LogicalGraphSchema.hpp"
#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/StorageBackend.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageTypes.hpp"
#include "graph/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgePublicationPreflight.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphFinalPublication.hpp"
#include "graph/storage/relocatable/schema/ConstraintDefinition.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "graph/storage/relocatable/metadata/MetadataSnapshotRegistry.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/LogicalQueryScratch.hpp"
#include "graph/storage/relocatable/index/ScalarIndexTreeReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/schema/TokenDefinition.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableMaintenanceService.hpp"
#include "graph/storage/relocatable/maintenance/VectorIndexStateReclaimer.hpp"

namespace graph::storage {

	class NativeBulkSessionLease;

	struct RelocatableStorageIOStats {
		StorageIOStats data;
		BufferPoolStats buffer_pool;

		constexpr bool operator==(const RelocatableStorageIOStats &) const = default;
	};

	class EntityPageStore;
	class EntityLabelSetStore;
	class NativeGraphWritePlanner;
	class NativeGraphMutationExecutor;
	class NativeGraphWriteTransaction;
	class NativeBulkRunStore;
	struct NativeGraphWriteCommitResult;
	struct NativeGraphLogicalIdReservation;
	class PageBackedGraphReadAdapter;
	class BTreePageStore;
	class LargeValueStore;
	class PhysicalSegmentAllocator;
	class PropertyCollectionRootStore;
	class SlottedPagePool;
	class SystemCatalog;
	class VectorCodebookStore;
	class VectorManifestStore;
	class VectorPageStore;
	class RelocatableMetadataStore;
	class RelocatableStorageBackend;
	class RelocatableLogicalGraphReadSnapshot;
	class OwnedRelocatableBackendFile;

	struct RelocatableStorageBackendOptions {
		std::filesystem::path data_path;
		OpenMode open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
		// This is a byte limit, not a historical cache-entry count. The pool owns
		// only complete 4 KiB raw pages and rounds down to that page granularity.
		size_t buffer_pool_bytes = 64U * 1024U * 1024U;
		// Full integrity scans are deliberately opt-in: they are O(database size)
		// and belong to cutover, repair, and operator verification workflows.
		bool verify_integrity_on_open = false;
        // Absolute logical file length, including durable data and scratch.
        // Growth beyond this finite ceiling fails before cache or disk mutation.
        uint64_t maximum_database_file_bytes = UINT64_C(1024) * 1024U * 1024U * 1024U;
        // Per-scan MAINTENANCE grant; resource exhaustion fails the read-only scan.
        size_t maximum_integrity_workspace_bytes = 8U * 1024U * 1024U;
        // Per explicit reclamation pass, admitted before publication.
        size_t maximum_reclaim_workspace_bytes = 8U * 1024U * 1024U;
		// Cost-admitted ROW chunks with bounded compression and RAW fallback;
		// callers can retain uniform RAW placement by disabling this policy.
		bool adaptive_property_row_chunks = true;
		// Internal query-only permission, independent of graph write permissions.
		bool query_scratch_enabled = false;
		// Aggregate private scratch extents across sessions of this database file.
		size_t maximum_database_query_scratch_bytes = size_t{1024} * 1024U * 1024U;
		// The immutable schema registry is loaded once when a writable backend
		// opens. Keep this bounded so a malformed file cannot force an unbounded
		// allocation before the graph-write boundary is established.
		size_t maximum_index_definitions = 65536U;
		// Constraint definitions are read before every native write fence and must
		// remain bounded independently from public index metadata.
		size_t maximum_constraint_definitions = 65536U;
		// A hash bucket is verified by canonical values, not trusted as equality.
		// This explicit cap prevents an adversarial collision set from turning one
		// uniqueness check into unbounded point-lookup work.
		size_t maximum_constraint_support_collisions = 64U;
		// Online scalar-index construction is externally sorted and bounded. These
		// limits protect CREATE INDEX from turning one client request into an
		// unbounded memory, temporary-file, or segment-allocation operation.
		size_t maximum_online_index_entries = 10'000'000U;
		// DDL must inspect all live Nodes before it can prove that a historical
		// index is complete. This bound is intentionally independent from the
		// output-entry limit because labels and properties can be sparse.
		size_t maximum_online_index_scan_nodes = 10'000'000U;
		size_t maximum_online_index_pages = 1'000'000U;
		// Online Vector changes are merged through a compact exact delta overlay.
		// The threshold schedules a new immutable base; the hard ceiling keeps a
		// delayed or unavailable maintainer from making exact overlay scans
		// unbounded. The latter must also fit the configured delta-tree pages.
		size_t vector_delta_rebuild_threshold = 65'536U;
		size_t maximum_vector_delta_overlay_entries = 262'144U;
		size_t maximum_vector_delta_pages = 4'096U;
		// Ordinary graph adjacency uses bounded global Packed B+Tree range scans.
		size_t maximum_adjacency_read_edges = 10'000'000U;
		// Edge-only bulk transactions have their own transaction-wide record
		// ceiling. It bounds private scratch independently from a public batch.
		size_t maximum_bulk_edge_transaction_records = 10'000'000U;
		// Streamed Edge identities are leased incrementally. This is independent
		// from the transaction admission ceiling so a small transaction cannot
		// burn the complete transaction-sized logical-ID range. The effective
		// block is clamped to the transaction's remaining record allowance.
		size_t bulk_edge_id_lease_block_records = 65'536U;
		// One append is also a publication-gate work unit. Bound both cardinality
		// and encoded bytes so a caller cannot monopolize all native publications
		// with one transaction-sized call; larger ingests submit more appends.
		size_t maximum_bulk_edge_append_records = 65'536U;
		size_t maximum_bulk_edge_append_bytes = 64U * 1024U * 1024U;
		// The eventual streamed adjacency sorter retains at most one run of this
		// many encoded keys in memory. It is independent from the transaction
		// record ceiling because key size and Edge Properties are variable.
		size_t bulk_edge_sort_run_bytes = 8U * 1024U * 1024U;
		// One merge pass may open only this many private runs. Larger inputs reduce
		// in bounded fan-in groups inside the database file; no OS temporary file is
		// part of the ordinary graph-write protocol.
		size_t maximum_bulk_edge_sort_runs = 64U;
		// Bounds every new Entity, Property, Index, and directory page planned by a
		// single streamed bulk-edge candidate before its one Superblock publication.
		size_t maximum_bulk_edge_candidate_pages = 1'000'000U;
		// A streamed publication rebuilds bounded Catalog, global adjacency, and
		// LargeValue-content trees. This limits the complete final tree source,
		// including entries already visible before the transaction, rather than
		// merely limiting the incoming Edge rows.
		size_t maximum_bulk_edge_index_entries = 20'000'000U;
		// A logical read snapshot may retain a bounded number of native adjacency
		// cursors. Zero disables this optional acceleration.
		size_t maximum_snapshot_adjacency_cursors = 64U;
		size_t online_index_sort_run_bytes = 8U * 1024U * 1024U;
		size_t maximum_online_index_sort_runs = 64U;
		// Immutable Codebooks are shared by every query for one Vector index.
		// Bound both dimensions independently; setting both to zero disables the
		// cache, while a one-sided zero is rejected as an ambiguous configuration.
		size_t maximum_vector_codebook_cache_bytes = 64U * 1024U * 1024U;
		size_t maximum_vector_codebook_cache_entries = 64U;
		// Manual PQ training reads every authoritative raw record before publishing
		// a complete derived base generation. These independent ceilings bound its
		// CPU, memory, and derived-page footprint even when the caller supplies
		// larger limits.
		size_t maximum_vector_training_records = 1'000'000U;
		size_t maximum_vector_training_pages = 1'000'000U;
		size_t maximum_vector_training_samples = 5'000U;
		uint32_t vector_training_iterations = 8U;
		// Exact adjacency construction is a deterministic small-index baseline.
		// Larger indexes use the bounded candidate-pruned builder, so one manual
		// rebuild cannot silently become an unbounded quadratic operation.
		size_t maximum_vector_adjacency_exact_records = 2'048U;
		uint16_t maximum_vector_adjacency_degree = 32U;
		size_t vector_adjacency_candidate_search_budget = 128U;
		float vector_adjacency_pruning_alpha = 1.2F;
		// Maintenance is opt-in. When configured, only explicitly enqueued typed
		// tasks run; the backend never guesses candidates by scanning payload bytes.
		std::optional<RelocatableMaintenanceOptions> maintenance;
	};

	// A snapshot carries backend provenance as well as its pinned metadata root.
	// This prevents a caller from using a valid-looking logical root from another
	// database to resolve records through this backend's physical storage.
	class RelocatableReadSnapshot final {
	public:
		RelocatableReadSnapshot() = default;
		~RelocatableReadSnapshot() = default;
		RelocatableReadSnapshot(const RelocatableReadSnapshot &) = delete;
		RelocatableReadSnapshot &operator=(const RelocatableReadSnapshot &) = delete;
		RelocatableReadSnapshot(RelocatableReadSnapshot &&) noexcept = default;
		RelocatableReadSnapshot &operator=(RelocatableReadSnapshot &&) noexcept = default;

		[[nodiscard]] bool isValid() const { return backend_ != nullptr && pin_.isValid(); }
		[[nodiscard]] const Superblock &superblock() const { return pin_.superblock(); }
		// Native read adapters need the same immutable metadata pin as the owning
		// logical snapshot; the reference is const so callers cannot release or
		// repoint the pin independently.
		[[nodiscard]] const MetadataSnapshotPin &metadataSnapshot() const { return pin_; }
		void release() {
			pin_.release();
			backend_ = nullptr;
		}

	private:
		friend class RelocatableStorageBackend;
		RelocatableReadSnapshot(const RelocatableStorageBackend *backend, MetadataSnapshotPin pin) :
			backend_(backend), pin_(std::move(pin)) {}

		const RelocatableStorageBackend *backend_ = nullptr;
		MetadataSnapshotPin pin_;
	};

	class RelocatableStorageBackend final : public StorageBackend,
									  public LogicalGraphReadBackend,
									  public LogicalGraphStatisticsBackend,
									  public LogicalGraphAdministrationBackend,
									  public LogicalGraphMaintenanceController,
									  public LogicalGraphVectorSearchBackend,
									  public LogicalGraphVectorMaintenanceBackend,
									  public LogicalGraphWriteBackend,
									  public LogicalGraphTransactionalBackend,
									  public LogicalGraphSchemaBackend,
									  private PageBackedGraphNameResolver {
	public:
		explicit RelocatableStorageBackend(RelocatableStorageBackendOptions options);
		// Atomically creates one database file, initializes one complete native
		// Superblock generation, and
		// returns a backend configured for a normal writable open.
		[[nodiscard]] static std::shared_ptr<RelocatableStorageBackend> initializeNew(RelocatableStorageBackendOptions options);
		~RelocatableStorageBackend() override;
		RelocatableStorageBackend(const RelocatableStorageBackend &) = delete;
		RelocatableStorageBackend &operator=(const RelocatableStorageBackend &) = delete;

		void open() override;
		void close() override;
		void flush() override;
		[[nodiscard]] bool isOpen() const override { return metadata_ != nullptr; }
		[[nodiscard]] bool isReadOnly() const override {
			return options_.open_mode == OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
		}
		[[nodiscard]] StorageFormatKind formatKind() const override { return StorageFormatKind::SFM_RELOCATABLE; }
		[[nodiscard]] StorageBackendCapabilities capabilities() const override {
			return {true, true, true, true, false};
		}

		[[nodiscard]] RelocatableReadSnapshot openSnapshot() const;
		// The generic logical fence stays opaque above the storage boundary. Its
		// relocatable encoding compares graph-visible root identities exactly, while
		// deliberately excluding allocator high-water marks and token registries.
		[[nodiscard]] static LogicalGraphMutationFence makeLogicalGraphMutationFence(const Superblock &superblock);
		[[nodiscard]] static LogicalGraphSchemaIdentity makeLogicalGraphSchemaIdentity(const Superblock &superblock);
		// The format-independent direct-write bridge resolves logical names before
		// pinning the graph transaction, then publishes each complete batch in one
		// metadata COW generation. It intentionally does not expose physical write
		// placement through the public storage boundary.
		// Builds missing global label/type member trees in one native publication.
		// Existing definitions/roots are reused; no implicit rebuild on reopen.
		void rebuildLogicalTokenCardinalityStatistics(const std::function<void()> &checkpoint = {});
		[[nodiscard]] std::vector<uint64_t> ensureLogicalMembershipIndexes(
				const std::function<void()> &checkpoint = {});
		[[nodiscard]] std::vector<uint64_t>
		createLogicalNodes(const std::vector<LogicalGraphNodeCreateRequest> &requests) override;
		[[nodiscard]] std::vector<uint64_t>
		createLogicalEdges(const std::vector<LogicalGraphEdgeCreateRequest> &requests) override;
		void replaceLogicalNodeProperties(const std::vector<LogicalGraphPropertyReplaceRequest> &requests) override;
		void replaceLogicalEdgeProperties(const std::vector<LogicalGraphPropertyReplaceRequest> &requests) override;
		void deleteLogicalNodes(const std::vector<LogicalGraphNodeDeleteRequest> &requests) override;
		void deleteLogicalEdges(const std::vector<LogicalGraphEdgeDeleteRequest> &requests) override;
		[[nodiscard]] LogicalGraphMutationBatchResult executeLogicalGraphMutationBatch(
				const LogicalGraphMutationBatch &batch,
				std::optional<LogicalGraphMutationFence> expectedFence = std::nullopt) override;
		[[nodiscard]] LogicalGraphMutationBatchResult executeLogicalGraphMutationBatchWithReadControl(
			const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence,
			const LogicalGraphMutationReadControl &control) override;
		[[nodiscard]] LogicalGraphMutationBatchResult executeLogicalGraphMutationBatchWithPropertyPatches(
			LogicalGraphReadSnapshot &snapshot, const LogicalGraphMutationBatch &companionBatch,
			std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
			const LogicalGraphMutationFence &expectedFence, const LogicalGraphMutationReadControl &control = {}) override;

		// Internal logical-transaction bridge: return durable IDs before exposing a
		// provisional result, so the executor can later adopt the same ranges.
		[[nodiscard]] NativeGraphLogicalIdReservation reserveLogicalGraphCreationIds(uint64_t nodeCount,
																		 uint64_t edgeCount);
		// A streamed bulk-Edge transaction acquires bounded durable capabilities
		// incrementally. Every returned range remains exact so the final publisher
		// can claim it once and rollback can abandon it without making IDs reusable.
		[[nodiscard]] NativeGraphLogicalIdReservation reserveNativeBulkEdgeSessionIds(uint64_t edgeCount);
		void abandonLogicalGraphCreationIds(const NativeGraphLogicalIdReservation &reservation) noexcept;
		void abandonLogicalGraphCreationIds(std::span<const NativeGraphLogicalIdLeaseRange> nodes,
			std::span<const NativeGraphLogicalIdLeaseRange> edges) noexcept;
		// Internal transaction-only bridge for typed private scratch. It is kept on
		// the relocatable backend rather than the format-independent logical API so
		// no public caller can obtain an allocator, SegmentId, or metadata handle.
		[[nodiscard]] uint64_t allocateNativeBulkScratchOwner();
		// A streamed session holds this lease across durable scratch allocation and
		// cleanup so another process cannot publish overlapping allocator state.
		[[nodiscard]] std::unique_ptr<NativeBulkSessionLease> tryAcquireNativeBulkSessionLease();
		[[nodiscard]] std::unique_ptr<NativeBulkRunStore> createNativeBulkRunStore();
		[[nodiscard]] bool nativeQueryScratchEnabled() const {return options_.query_scratch_enabled && !isReadOnly();}
		[[nodiscard]] std::unique_ptr<LogicalQueryScratchSession> openNativeQueryScratchSession(size_t maximumDiskBytes,size_t maximumFragments);
        [[nodiscard]] std::unique_ptr<LogicalQueryScratchSession> openNativeQueryScratchSession(
            size_t maximumDiskBytes, size_t maximumFragments,
            std::shared_ptr<query::execution::detail::ResourceAccount> queryAccount);

		[[nodiscard]] Generation currentNativeMetadataGeneration() const;
		[[nodiscard]] LogicalGraphMutationFence currentNativeLogicalGraphMutationFence() const;
		// Internal transaction bridge; callers must not expose reads before choosing
		// this bounded blind-write path. All other transactions retain their fence.
		[[nodiscard]] LogicalGraphMutationBatchResult executeNativeBlindNodePropertyReplacements(
			const LogicalGraphMutationBatch &, const RelocatableReadSnapshot &, const LogicalGraphMutationFence &);

		// Internal streamed-transaction bridge. It pauses a service before the
		// caller takes the native publication gate, avoiding an in-flight worker
		// deadlock on that same gate.
		[[nodiscard]] bool quiesceNativeMaintenanceForExternalRebase();
		void refreshNativeMetadataAfterExternalPublication(bool restoreMaintenancePaused = false);
		[[nodiscard]] LogSequenceNumber nextNativeBulkScratchCheckpointLsn() const;
		// Finalizes one already sealed, allocator-managed Edge source. The caller
		// supplies no physical placement authority; this backend owns every pool,
		// candidate segment, and one-generation publication decision.
		void commitNativeStreamedBulkEdges(NativeBulkRunStore &scratch,
										 NativeBulkRunFragmentLocation manifestLocation,
										 const NativeGraphLogicalIdReservation &reservation,
										 const NativeStreamedGraphPublicationPlan &plan,
										 const LogicalGraphMutationFence &expectedFence);
		// Serializes one bounded allocator-managed scratch publication with graph,
		// DDL, and maintenance publication. The lock must never escape one append,
		// finish, or cleanup operation into the bulk transaction lifetime.
		[[nodiscard]] std::unique_lock<std::mutex> acquireNativePublicationGate();
		// An ambiguous scratch cleanup leaves allocator state that can only be
		// reconciled by writable-open recovery. Reject later writes on this handle
		// rather than allocating over
		// an unverified tail.
		void requireReopenAfterNativeBulkStagingFailure() noexcept;
		[[nodiscard]] bool hasIndeterminateWriteOutcome() const noexcept {
			return requires_reopen_after_indeterminate_write_.load(std::memory_order_acquire);
		}
		[[nodiscard]] NativeBulkEdgePublicationPreflightOptions nativeBulkEdgePublicationPreflightOptions() const {
			// Private candidates are fixed-size segments. Plan with that same
			// payload limit so preflight never underestimates the number of runs
			// that final private materialization must retain.
			return {options_.maximum_bulk_edge_transaction_records,
					std::min(options_.bulk_edge_sort_run_bytes,
							 static_cast<size_t>(RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES)),
					options_.maximum_bulk_edge_sort_runs, options_.maximum_bulk_edge_candidate_pages};
		}
		[[nodiscard]] size_t maximumNativeBulkEdgeTransactionRecords() const {
			return options_.maximum_bulk_edge_transaction_records;
		}
		[[nodiscard]] size_t nativeBulkEdgeIdLeaseBlockRecords() const {
			return std::min(options_.bulk_edge_id_lease_block_records,
							options_.maximum_bulk_edge_transaction_records);
		}
		[[nodiscard]] size_t maximumNativeBulkEdgeAppendRecords() const {
			return std::min(options_.maximum_bulk_edge_append_records,
							options_.maximum_bulk_edge_transaction_records);
		}
		[[nodiscard]] size_t maximumNativeBulkEdgeAppendBytes() const {
			return options_.maximum_bulk_edge_append_bytes;
		}
		[[nodiscard]] std::unique_ptr<LogicalGraphTransaction> beginLogicalGraphTransaction(bool readOnly) override;
		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override;
        // Explicit laboratory reference-path control. Performs a bounded global
        // candidate integrity scan at setup, outside query planning/point reads.
        // The returned snapshot retains its source pin. No automatic adoption
        // or write authority; only a physical read-only backend may open it.
        [[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openCoveredAdjacencyCandidateReadSnapshot(
            uint64_t indexId, size_t maximumEdges, size_t maximumEdgeMetadataPages,
            std::shared_ptr<std::pmr::memory_resource> memory,
            const CancellationCheckpoint *cancellation = nullptr) const;

		[[nodiscard]] std::optional<LogicalGraphSchemaIdentity> logicalPlanCacheIdentity() const override;
        [[nodiscard]] std::shared_ptr<std::pmr::memory_resource> logicalPlanCacheMemoryOwner() const override { return native_cache_memory_; }
		[[nodiscard]] std::shared_ptr<query::execution::detail::DatabaseResourceBudget> queryResourceDomain() const override {
			requireOpen(); return resource_domain_;
		}
		[[nodiscard]] std::optional<LogicalGraphCardinalityStatistics>
		readLogicalGraphCardinalityStatistics() const override;
		[[nodiscard]] std::optional<uint64_t>
		estimateLogicalNodeCardinality(std::span<const std::string> labels) const override;
		[[nodiscard]] LogicalGraphPlanningStatistics readLogicalPlanningStatistics(
			std::span<const std::vector<std::string>> labels) const override;
        [[nodiscard]] LogicalGraphPlanningStatistics readLogicalPlanningStatistics(
            std::span<const std::vector<std::string>> labels, std::span<const std::string> types) const override;

		[[nodiscard]] std::unique_ptr<LogicalGraphVectorSearchSnapshot>
		openLogicalGraphVectorSearchSnapshot() const override;
		// Exact bounded refinement is the semantic baseline. Derived PQ/adjacency
		// search may reduce candidates later, but it must preserve this snapshot
		// validation and deterministic result ordering.
		[[nodiscard]] LogicalGraphVectorSearchResult
		searchLogicalVector(const RelocatableReadSnapshot &snapshot, const LogicalGraphVectorSearchRequest &request) const;
		[[nodiscard]] std::optional<LogicalGraphVectorIndexDescriptor>
		describeLogicalVectorIndex(const RelocatableReadSnapshot &snapshot,
								   const LogicalGraphVectorSearchRequest &request) const;
		[[nodiscard]] std::optional<LogicalGraphVectorSearchHit>
		scoreLogicalVectorExactOverlayCandidate(const RelocatableReadSnapshot &snapshot,
											const LogicalGraphVectorSearchRequest &request,
											const LogicalGraphVectorIndexDescriptor &index,
											const LogicalGraphNode &node) const;
		[[nodiscard]] std::unique_ptr<PreparedLogicalGraphVectorTraining>
		prepareLogicalVectorTraining(const LogicalGraphVectorTrainingRequest &request) override;
		[[nodiscard]] LogicalGraphNode readNode(const RelocatableReadSnapshot &snapshot, uint64_t nodeId) const;
		[[nodiscard]] LogicalGraphEdge readEdge(const RelocatableReadSnapshot &snapshot, uint64_t edgeId) const;
		[[nodiscard]] bool containsLogicalNode(const RelocatableReadSnapshot &snapshot, uint64_t nodeId,
																				 NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<bool> containsLogicalNodesSorted(const RelocatableReadSnapshot &snapshot,
																				 std::span<const uint64_t> nodeIds,
																				 NativeReadSession *session = nullptr) const;
		// Resolves a complete exact Node match through a compatible UNIQUE/NODE KEY
		// support tree. nullopt means no compatible durable constraint exists and
		// callers must retain their bounded scan path.
		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByExactProperties(
				const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
				const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
				size_t maximumPropertyEntries) const;
		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByPropertyRange(
				const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
				const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries) const;
		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByCompositePropertyRange(
				const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
				const LogicalGraphCompositePropertyRange &request, size_t maximumNodes,
				size_t maximumPropertyEntries) const;
		// Directory traversal is ordered by stable logical ID and has an explicit
		// record bound and never materializes an unbounded graph result.
		void visitNodes(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
						const std::function<void(const LogicalGraphNode &)> &visitor) const;
		void visitEdges(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
						const std::function<void(const LogicalGraphEdge &)> &visitor) const;
		[[nodiscard]] bool visitNodesUntil(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
												  const std::function<bool(const LogicalGraphNode &)> &visitor) const;
		[[nodiscard]] bool visitEdgesUntil(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
												  const std::function<bool(const LogicalGraphEdge &)> &visitor) const;
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(const RelocatableReadSnapshot &snapshot, uint64_t afterNodeId,
																				 size_t maximumRecords, size_t maximumMetadataPages) const;
		// The continuation contains stable logical Edge IDs only and is revalidated
		// against the caller's pinned snapshot before resuming.
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
				const RelocatableReadSnapshot &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const;
		void visitAdjacentEdges(const RelocatableReadSnapshot &snapshot, uint64_t nodeId,
												LogicalGraphReadDirection direction, size_t maximumEdges,
												const std::function<void(const LogicalGraphEdge &)> &visitor) const;
		[[nodiscard]] bool visitAdjacentEdgesUntil(
				const RelocatableReadSnapshot &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
				size_t maximumEdges, const std::function<bool(const LogicalGraphEdge &)> &visitor) const;
		[[nodiscard]] std::optional<uint32_t> findPropertyKeyToken(const RelocatableReadSnapshot &snapshot,
																   std::string_view key) const;
		[[nodiscard]] std::optional<std::string> findPropertyKey(const RelocatableReadSnapshot &snapshot,
																 uint32_t token) const;
		[[nodiscard]] std::optional<TokenDefinition> findSchemaToken(const RelocatableReadSnapshot &snapshot,
																	 uint64_t tokenId) const;
		[[nodiscard]] std::optional<uint64_t> findSchemaTokenId(const RelocatableReadSnapshot &snapshot,
																						std::string_view name) const;
		[[nodiscard]] std::optional<uint64_t> findRelationshipTypeTokenId(const RelocatableReadSnapshot &snapshot,
																								std::string_view name) const;
		[[nodiscard]] utils::OwnedStdVector<IndexDefinition> readIndexDefinitions(const RelocatableReadSnapshot &snapshot,
																		size_t maximumDefinitions) const;
		[[nodiscard]] LogicalGraphIndexDefinition
		createLogicalNodeIndex(const LogicalGraphNodeIndexCreateRequest &request) override;
		[[nodiscard]] LogicalGraphIndexDefinition createLogicalNodeIndexWithCheckpoint(
			const LogicalGraphNodeIndexCreateRequest &request, const std::function<void()> &checkpoint,
			const std::function<void(size_t)> &propertyAdmission = {}) override;
		[[nodiscard]] std::vector<LogicalGraphIndexDefinition> createLogicalNodeIndexesWithCheckpoint(
			std::span<const LogicalGraphNodeIndexCreateRequest> requests, const std::function<void()> &checkpoint,
			const std::function<void(size_t)> &propertyAdmission = {}) override;
		[[nodiscard]] LogicalGraphIndexDefinition createLogicalNodeVectorIndexWithCheckpoint(
			const LogicalGraphNodeVectorIndexCreateRequest &request, const std::function<void()> &checkpoint,
			const std::function<void(size_t)> &propertyAdmission = {}) override;
		[[nodiscard]] LogicalGraphConstraintDefinition createLogicalConstraintWithCheckpoint(
			const LogicalGraphConstraintCreateRequest &request, const std::function<void()> &checkpoint,
			const std::function<void(size_t)> &propertyAdmission = {}) override;

		[[nodiscard]] std::vector<LogicalGraphIndexDefinition>
		createLogicalNodeIndexes(std::span<const LogicalGraphNodeIndexCreateRequest> requests) override;
		[[nodiscard]] LogicalGraphIndexDefinition
		createLogicalNodeVectorIndex(const LogicalGraphNodeVectorIndexCreateRequest &request) override;
		[[nodiscard]] bool dropLogicalIndexByName(std::string_view name) override;
		[[nodiscard]] bool dropLogicalNodeIndexBySchema(std::string_view label, std::string_view propertyKey) override;
        [[nodiscard]] utils::OwnedStdVector<LogicalGraphIndexDefinition> listOwnedLogicalIndexes(size_t maximumIndexes) const override;
        [[nodiscard]] utils::OwnedStdVector<LogicalGraphConstraintDefinition> listOwnedLogicalConstraints(size_t maximumConstraints) const override;
		[[nodiscard]] std::vector<LogicalGraphIndexDefinition> listLogicalIndexes(size_t maximumIndexes) const override;
		[[nodiscard]] LogicalGraphConstraintDefinition
		createLogicalConstraint(const LogicalGraphConstraintCreateRequest &request) override;
		[[nodiscard]] bool dropLogicalConstraintByName(std::string_view name) override;
		[[nodiscard]] std::vector<LogicalGraphConstraintDefinition>
		listLogicalConstraints(size_t maximumConstraints) const override;
		// DDL publishes logical removal first. This explicit maintenance entry point
		// subsequently retires obsolete scalar trees with caller-selected bounds, so
		// a cleanup failure never changes the durable outcome of DROP INDEX.
		[[nodiscard]] ScalarIndexTreeReclaimReport
		reclaimObsoleteScalarIndexTrees(const ScalarIndexTreeReclaimOptions &options,
										const MetadataPublishOptions &publishOptions = {});
		// Vector DROP publishes logical invisibility first. This explicit bounded
		// maintenance entry point retires its physical dependency graph afterwards.
		[[nodiscard]] VectorIndexStateReclaimReport
		reclaimObsoleteVectorIndexState(const VectorIndexStateReclaimOptions &options,
										const MetadataPublishOptions &publishOptions = {});
		// Graph mutation removes an owner reference before it reclaims immutable
		// LargeValue storage. This explicit bounded maintenance entry point closes
		// that lifecycle without coupling user-visible DELETE to physical cleanup.
		[[nodiscard]] LargeValueReachabilityReclaimReport
		reclaimUnreachableLargeValues(const LargeValueReachabilityReclaimOptions &options,
								  const MetadataPublishOptions &publishOptions = {});
		[[nodiscard]] bool maintenanceEnabled() const { return maintenance_ != nullptr; }
		[[nodiscard]] bool nativeMaintenanceAvailable() const noexcept override { return maintenance_ != nullptr; }
		[[nodiscard]] bool nativeMaintenanceEnabled() const override;
		void setNativeMaintenanceEnabled(bool enabled) override;
		void pauseMaintenance();
		void resumeMaintenance();
		[[nodiscard]] size_t cancelQueuedMaintenance();
		[[nodiscard]] uint64_t enqueuePropertyPageCompaction(SegmentId source, SegmentId destination,
															 uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueBTreePageCompaction(SegmentId source, SegmentId destination, uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueVectorPageCompaction(SegmentId source, SegmentId destination,
														   uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueVectorManifestCompaction(SegmentId source, SegmentId destination,
															   uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueEntityPageCompaction(SegmentId source, SegmentId destination,
																 uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueLargeValuePageCompaction(SegmentId source, SegmentId destination, uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueLargeValueReachabilityReclamation(
				const LargeValueReachabilityReclaimOptions &options);
		[[nodiscard]] uint64_t enqueueCodebookExtentRelocation(uint64_t codebookId, ExtentId extentId,
															   PhysicalSlotId firstDestination,
															   PhysicalSlotId lastDestination);
		[[nodiscard]] RelocatableMaintenanceMetrics maintenanceMetrics() const;
		[[nodiscard]] std::exception_ptr maintenanceFailure() const;
		[[nodiscard]] RelocatableIntegrityReport verifyIntegrity() const;

		[[nodiscard]] const RelocatableStorageBackendOptions &options() const { return options_; }
		[[nodiscard]] RelocatableStorageIOStats ioStats() const;
		void resetIoStats();
		// Durable configuration records are metadata, while I/O statistics remain
		// process-local observations exposed through ioStats()/resetIoStats().
		[[nodiscard]] std::optional<PropertyValue> findConfiguration(std::string_view name) const override;
		[[nodiscard]] std::vector<LogicalGraphConfiguration> readConfigurations(size_t maximumRecords) const override;
		void setConfiguration(std::string_view name, const PropertyValue &value) override;
		[[nodiscard]] std::vector<LogicalGraphRuntimeStatistic> runtimeStatistics() const override;
		void resetRuntimeStatistics() override;

	private:
		friend class RelocatableLogicalGraphReadSnapshot;
		[[nodiscard]] LogicalGraphMutationBatchResult executeNativeMutationBatch(
			const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence,
			const LogicalGraphMutationReadControl &control,
			std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
			const std::function<void(const MetadataSnapshotPin &)> &rebaseValidation = {});
		void requireOpen() const;
		void requireOwnedSnapshot(const RelocatableReadSnapshot &snapshot) const;
		[[nodiscard]] std::string resolveNodeLabelToken(const MetadataSnapshotPin &snapshot,
																	 uint64_t tokenId) const override;
		[[nodiscard]] std::string resolveRelationshipTypeToken(const MetadataSnapshotPin &snapshot,
																			uint64_t tokenId) const override;
		[[nodiscard]] std::string resolveRelationshipTypeTokenWithNameAdmission(const MetadataSnapshotPin &snapshot,
			uint64_t tokenId, const std::function<void(size_t)> &admission) const override;
		[[nodiscard]] std::string resolvePropertyKeyWithNameAdmission(const MetadataSnapshotPin &snapshot,
			uint32_t tokenId, const std::function<void(size_t)> &admission) const override;
		void prefetchPropertyKeyNames(const MetadataSnapshotPin &snapshot, std::span<const uint32_t> tokens,
			const std::function<void(uint32_t, std::string_view)> &consume) const override;
		[[nodiscard]] std::string resolvePropertyKey(const MetadataSnapshotPin &snapshot,
																				  uint32_t tokenId) const override;
		[[nodiscard]] std::optional<uint32_t> resolvePropertyToken(const MetadataSnapshotPin &snapshot,
																			 std::string_view name) const override;
		[[nodiscard]] std::optional<uint64_t> resolveNodeLabelName(const MetadataSnapshotPin &snapshot,
																		  std::string_view name) const override;
		[[nodiscard]] utils::OwnedStdVector<IndexDefinition> readAllIndexDefinitions(const MetadataSnapshotPin &snapshot,
																									   size_t maximumDefinitions) const;
		[[nodiscard]] utils::OwnedStdVector<ConstraintDefinition>
		readAllConstraintDefinitions(const MetadataSnapshotPin &snapshot, size_t maximumDefinitions) const;
		// A failed final publication fence can leave either the old or the newly
		// published Superblock durable. Keep this distinction at the transaction
		// boundary so validation and planning failures do not unnecessarily poison
		// a writable handle, while an ambiguous publication never permits a retry
		// on that same handle.
		[[nodiscard]] NativeGraphWriteCommitResult
		commitNativeGraphWriteTransaction(NativeGraphWriteTransaction &transaction);
		void markReopenRequiredIfPublicationAdvanced(const Superblock &before) noexcept;
		void requireNoIndeterminateWrite() const;
		// The public training entry point acquires native_publication_mutex_. The
		// maintenance service already holds that same gate, so its typed rebuild
		// executor must use this non-reentrant implementation instead.
		[[nodiscard]] LogicalGraphVectorTrainingResult
		trainLogicalVectorUnderPublicationLock(const LogicalGraphVectorTrainingRequest &request);
		// The native exact lookup validates a support-tree candidate against the
		// same snapshot-pinned Entity pages and System Catalog that created it.
		// It deliberately never falls back to the retired fixed-record path.
		[[nodiscard]] std::optional<bool> visitNativeEdgeTypeCandidates(
				const MetadataSnapshotPin &snapshot, uint64_t typeToken, size_t maximumRecords,
				const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session) const;
		[[nodiscard]] std::optional<bool> visitNativeNodeLabelCandidates(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				size_t maximumRecords, const std::function<bool(const EntityRef &, bool)> &visitor,
				NativeReadSession *session) const;
		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNativeNodesByExactProperties(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
				size_t maximumPropertyEntries, NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::optional<IndexCandidateBatch> findNativeNodeCandidatesByExactProperties(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
				 size_t maximumPropertyEntries, NativeReadSession *session = nullptr,
				IndexCandidateBatchVisitor *visitor = nullptr, bool *visitor_completed = nullptr,
				size_t maximumCandidateBytes = std::numeric_limits<size_t>::max()) const;
		[[nodiscard]] std::optional<bool> visitNativeNodeCandidatesByExactProperties(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
				size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor,
				NativeReadSession *session = nullptr,
				size_t maximumCandidateBytes = std::numeric_limits<size_t>::max()) const;
		[[nodiscard]] std::optional<IndexCandidateBatch> findNativeNodeCandidatesByPropertyRange(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries,
				NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::optional<bool> visitNativeNodeCandidatesByPropertyRange(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries,
				const IndexCandidateBatchVisitor &visitor, NativeReadSession *session = nullptr,
				size_t maximumCandidateBytes = std::numeric_limits<size_t>::max()) const;
		[[nodiscard]] std::optional<IndexCandidateBatch> findNativeNodeCandidatesByCompositePropertyRange(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				const LogicalGraphCompositePropertyRange &request, size_t maximumNodes,
				 size_t maximumPropertyEntries, NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::optional<bool> visitNativeNodeCandidatesByCompositePropertyRange(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				const LogicalGraphCompositePropertyRange &request, size_t maximumNodes,
				size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor,
				NativeReadSession *session = nullptr,
				size_t maximumCandidateBytes = std::numeric_limits<size_t>::max()) const;
		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNativeNodesByPropertyRange(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries,
				NativeReadSession *session = nullptr, IndexCandidateBatch *candidateOutput = nullptr,
				IndexCandidateBatchVisitor *visitor = nullptr, bool *visitor_completed = nullptr,
				size_t maximumCandidateBytes = std::numeric_limits<size_t>::max()) const;
		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNativeNodesByCompositePropertyRange(
				const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
				const LogicalGraphCompositePropertyRange &request, size_t maximumNodes,
				size_t maximumPropertyEntries, NativeReadSession *session = nullptr,
				IndexCandidateBatch *candidateOutput = nullptr, IndexCandidateBatchVisitor *visitor = nullptr,
				bool *visitor_completed = nullptr,
				size_t maximumCandidateBytes = std::numeric_limits<size_t>::max()) const;
		void verifyConstraintSupportTrees(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] RelocatableMaintenanceService &requireMaintenance();
		[[nodiscard]] const RelocatableMaintenanceService &requireMaintenance() const;
		void initializeNativeMaintenanceService();
		void releaseResources() noexcept;

		RelocatableStorageBackendOptions options_;
		// A successful open establishes this complete common component domain;
		// writable opens additionally install both native writers.
		// A construction failure rolls the whole domain back through releaseResources().
		std::unique_ptr<OwnedRelocatableBackendFile> data_file_;
		std::shared_ptr<query::execution::detail::DatabaseResourceBudget> resource_domain_;
        std::shared_ptr<const uint64_t> database_file_limit_;
        std::shared_ptr<std::pmr::memory_resource> native_cache_memory_;
		std::unique_ptr<RelocatableMetadataStore> metadata_;
		std::unique_ptr<PhysicalSegmentAllocator> physical_;
		std::unique_ptr<VectorCodebookStore> vector_codebooks_;
		std::unique_ptr<SlottedPagePool> slotted_pages_;
		std::unique_ptr<BTreePageStore> btree_pages_;
		// Native Entity pages are constructed for every open so readers and writers
		// share one metadata-domain-validated storage boundary during cutover.
		std::unique_ptr<EntityPageStore> native_entities_;
		std::unique_ptr<EntityLabelSetStore> native_labels_;
		std::unique_ptr<NativeGraphWritePlanner> native_graph_write_planner_;
		std::unique_ptr<NativeGraphMutationExecutor> native_graph_mutation_executor_;
		std::unique_ptr<PageBackedGraphReadAdapter> native_graph_reads_;
		std::unique_ptr<PageBackedLogicalGraphReadAdapter> native_logical_graph_reads_;
		std::unique_ptr<SystemCatalog> system_catalog_;
		std::unique_ptr<LargeValueStore> large_values_;
		std::unique_ptr<PropertyCollectionRootStore> properties_;
		std::unique_ptr<VectorPageStore> vector_pages_;
		std::unique_ptr<VectorManifestStore> vector_manifests_;
		std::unique_ptr<RelocatableMaintenanceService> maintenance_;
		// Token definitions are immutable within a pinned metadata generation.
		// Caching their public names avoids one directory traversal per entity in
		// scans without leaking names across a later generation.
		// DDL calls are serialized independently from graph writes. Native
		// transactions still fence the final publication against every writer.
		// All native graph, DDL, and maintenance publications share one gate. This
		// serializes typed page relocation with the generation that created its
		// logical Entity/Property/Index references.
		mutable std::mutex native_publication_mutex_;
		std::mutex query_scratch_registry_mutex_;
		std::shared_ptr<NativeBulkSessionLease> query_scratch_lease_;
		size_t query_scratch_sessions_ = 0U;
		// Scratch owners are process-local identities. Unpublished entries are
		// reclaimed on writable reopen, so this never becomes database state.
		std::atomic<uint64_t> next_bulk_scratch_owner_id_{1U};
		// An exception after attempted Superblock publication has an ambiguous
		// outcome. A caller must close/reopen and recover the newest valid root.
		std::atomic_bool requires_reopen_after_indeterminate_write_{false};
	};

} // namespace graph::storage
