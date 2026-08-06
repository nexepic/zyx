/**
 * @file NativeGraphWritePlanner.hpp
 * @brief Snapshot-pinned native Entity/Page graph write planning.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "graph/storage/relocatable/entity/NativeVectorMutationPlanner.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"

namespace graph::storage {

	class PropertyCollectionRootStore;
	class VectorManifestStore;
	class VectorPageStore;
	struct NativeGraphWritePlannerContext;

	// All enum members carry a prefix to remain safe on Windows builds.
	enum class NativeGraphWriteTransactionState : uint8_t {
		NGWTS_ACTIVE = 1,
		NGWTS_COMMITTED = 2,
		NGWTS_ROLLED_BACK = 3,
		NGWTS_CONFLICTED = 4,
		NGWTS_INDETERMINATE = 5,
	};

	// All enum members carry a prefix to remain safe on Windows builds.
	enum class NativeGraphWriteCommitOutcome : uint8_t {
		NGWCO_COMMITTED = 1,
		NGWCO_CONFLICTED = 2,
	};

	struct NativeGraphWriteCommitResult {
		NativeGraphWriteCommitOutcome outcome = NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED;
		Superblock superblock{};
		// Index IDs whose committed delta crossed the configured maintenance
		// threshold. The graph write remains durable; callers submit these IDs to
		// the maintenance service after observing the commit.
		std::vector<uint64_t> vector_rebuild_index_ids;
		[[nodiscard]] constexpr bool committed() const {
			return outcome == NativeGraphWriteCommitOutcome::NGWCO_COMMITTED;
		}
	};

	// Entity pools are shared by Node, Edge, and LabelSet pages. Property roots
	// stay in their own typed pool namespace and remain owner-keyed.
	struct NativeGraphWritePlannerOptions {
		PhysicalSlotId first_slot = PhysicalSlotId(1U);
		PhysicalSlotId last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
		uint64_t entity_pool_owner_id = 1U;
		// Bounds transaction-local topology inspection and prevents a malformed
		// adjacency chain from forcing an unbounded allocation during DETACH.
		size_t maximum_adjacency_read_edges = 10'000'000U;
		// Bounds a final global-adjacency root retirement. This is deliberately
		// independent of per-Node traversal limits because retirement walks the
		// complete shared tree, not one Node's range.
		size_t maximum_global_adjacency_tree_pages = 1'000'000U;
		// Bounds the two directional key mutations retained by one graph write.
		// The initial global tree uses the streaming bulk builder once this exceeds
		// a small transaction-local tree bootstrap.
		size_t maximum_global_adjacency_write_entries = 10'000'000U;
		// One durable high-water publication grants this many IDs to the shared
		// planner context. Unused IDs are intentionally never recycled, preserving
		// uniqueness across rollback and process crashes.
		size_t logical_id_lease_block_size = 4096U;
		// Bounds all Node or Edge identities retained by one transaction, including
		// cancelled branch reservations. This keeps lease planning O(bounded work).
		size_t maximum_logical_id_reservation = 10'000'000U;
		PropertyCollectionRootStoreOptions property_roots = {PhysicalSlotId(1U),
															 PhysicalSlotId(std::numeric_limits<uint64_t>::max()), 1U};
	};

	// Vector bootstrap receives the canonical bfloat16 payload, without the
	// persisted Vector-ID prefix. The transaction assigns that prefix from its
	// snapshot-pinned logical Vector-ID range before it builds either mapping
	// tree, so callers cannot manufacture durable Vector identities.
	struct NativeGraphVectorBootstrapRecord {
		uint64_t node_id = 0U;
		std::vector<std::byte> encoded_bfloat16;
	};

	struct NativeGraphVectorBootstrapOptions {
		PhysicalSlotId first_slot = PhysicalSlotId(1U);
		PhysicalSlotId last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
		size_t maximum_vectors = 0U;
		// Bounds the two immutable mapping trees, Raw pages, and the Manifest as
		// one DDL work unit. It prevents a corrupted/oversized bootstrap request
		// from retaining an unbounded candidate publication in memory.
		size_t maximum_pages = 0U;
	};

	struct NativeGraphVectorDeltaOptions {
		PhysicalSlotId first_slot = PhysicalSlotId(1U);
		PhysicalSlotId last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
		// Bounds one same-index Vector mutation batch, including all COW tail,
		// append, and fresh-pool Raw pages. Mapping-tree pages are separately
		// bounded by their native structural mutation plans.
		size_t maximum_records = 0U;
		size_t maximum_raw_pages = 0U;
		// Crossing the threshold schedules a base rebuild. The separate hard cap
		// keeps a delayed maintainer from allowing a long-lived exact overlay scan
		// to consume unbounded CPU or memory.
		size_t rebuild_threshold = 0U;
		size_t maximum_overlay_entries = 0U;
		size_t maximum_delta_pages = 0U;
	};

	class NativeGraphWriteTransaction final {
	public:
		NativeGraphWriteTransaction() = default;
		~NativeGraphWriteTransaction();
		NativeGraphWriteTransaction(const NativeGraphWriteTransaction &) = delete;
		NativeGraphWriteTransaction &operator=(const NativeGraphWriteTransaction &) = delete;
		NativeGraphWriteTransaction(NativeGraphWriteTransaction &&) noexcept;
		NativeGraphWriteTransaction &operator=(NativeGraphWriteTransaction &&) noexcept;

		[[nodiscard]] bool isActive() const;
		[[nodiscard]] NativeGraphWriteTransactionState state() const { return state_; }
		[[nodiscard]] const Superblock &baseSuperblock() const;
		// Native derived-state planners must read exactly the snapshot fenced by
		// this transaction, otherwise a support tree could describe a different
		// graph generation than the one that publishes its root.
		[[nodiscard]] const MetadataSnapshotPin &baseSnapshot() const;

		// IDs come from durable range leases. A returned ID is never reused, even if
		// the surrounding graph transaction rolls back or the process crashes.
		[[nodiscard]] uint64_t createNode(std::span<const uint64_t> labels);
		[[nodiscard]] uint64_t createEdge(uint64_t sourceNodeId, uint64_t targetNodeId, uint64_t typeId);
		// Adopts IDs leased by the logical transaction facade. Adoption is exact:
		// the graph transaction cannot mint a replacement range or use an ID that
		// was not durably granted to that logical transaction.
		void adoptLogicalIdReservation(NativeGraphLogicalIdReservation reservation);
		[[nodiscard]] uint64_t createNodeWithReservedId(uint64_t nodeId, std::span<const uint64_t> labels);
		[[nodiscard]] uint64_t createEdgeWithReservedId(uint64_t edgeId, uint64_t sourceNodeId, uint64_t targetNodeId,
															 uint64_t typeId);
		// Higher logical layers may reserve provisional IDs that were issued to a
		// cancelled branch. The reservation advances the durable high-water mark
		// at commit without creating an unreachable physical record.
		void reserveNodeIds(uint64_t count);
		void reserveEdgeIds(uint64_t count);
		// Traversal uses the transaction-local topology, including already staged
		// adjacency repairs, so DETACH validation cannot observe a stale snapshot.
		[[nodiscard]] std::vector<uint64_t> collectIncidentEdgeIds(uint64_t nodeId, size_t maximumEdges) const;
		[[nodiscard]] bool nodeHasIncidentEdges(uint64_t nodeId) const;
		void replaceNodeLabels(uint64_t nodeId, std::span<const uint64_t> labels);
		void replaceNodeProperty(PropertyCollectionRoot root);
		void replaceEdgeProperty(PropertyCollectionRoot root);
		void eraseNode(uint64_t nodeId);
		void eraseEdge(uint64_t edgeId);
		// Stage logical scalar-index operations. One Index is replayed through one
		// transaction-local B+Tree overlay, then published with graph state under
		// the same snapshot and Superblock generation.
		void stageScalarIndexUpsert(BTreePageStore &index, uint64_t indexId, std::span<const std::byte> key,
									uint64_t value);
		void stageScalarIndexErase(BTreePageStore &index, uint64_t indexId, std::span<const std::byte> key);
		// First publication reserves only fresh Index pages and makes the new root
		// visible with this graph transaction. Large DDL builds use the external
		// sorter path rather than retaining an unbounded entry set here.
		void stageScalarIndexCreate(BTreePageStore &index, uint64_t indexId,
									std::span<const BTreePageEntry> entries);
		void stageScalarIndexCreate(BTreePageStore &index, BTreeInitialTreeMutationPlan plan);
		// Deletes an entire derived tree through the enclosing native graph
		// generation. A bounded plan prevents corrupt metadata from causing an
		// unbounded traversal while DDL is being staged.
		void stageScalarIndexDrop(BTreePageStore &index, uint64_t indexId, size_t maximumPages);
		// Stages first-use Vector state exclusively through native typed pages. The
		// caller supplies a snapshot-derived graph scan; commit publishes the two
		// mapping roots, raw locations, manifest, schema definition, and graph
		// state in one generation.
		void stageVectorIndexBootstrap(BTreePageStore &mappingTrees, VectorPageStore &rawPages,
								   VectorManifestStore &manifests, const IndexDefinition &definition,
								   std::span<const NativeGraphVectorBootstrapRecord> records,
								   NativeGraphVectorBootstrapOptions options);
		// Stages an existing Vector index's semantic mapping transition. The caller
		// provides one deterministic same-index diff from NativeVectorMutationPlanner;
		// this boundary assigns new Vector IDs, retains source fences, and later
		// publishes mappings, Raw pages, locations, and Manifest together.
		void stageVectorIndexDelta(BTreePageStore &mappingTrees, VectorPageStore &rawPages,
								VectorManifestStore &manifests, const IndexDefinition &definition,
								std::span<const NativeVectorMutation> mutations,
								NativeGraphVectorDeltaOptions options);

		[[nodiscard]] NativeGraphWriteCommitResult commit(const MetadataPublishOptions &options = {});
		void rollback();

	private:
		friend class NativeGraphWritePlanner;

		struct NodeChange {
			NodeEntityRecord record;
			bool inserted = false;
			bool erased = false;
			std::optional<std::vector<uint64_t>> labels;
		};
		struct EdgeChange {
			EdgeEntityRecord record;
			bool inserted = false;
			bool erased = false;
		};
		struct StagedScalarIndexMutation {
			BTreePageStore *index = nullptr;
			uint64_t index_id = 0U;
			std::vector<BTreeScalarIndexOperation> operations;
			std::optional<BTreeStructuralMutationPlan> plan;
			std::optional<BTreeTreeRetirementPlan> retirement_plan;
		};
		struct StagedInitialScalarIndexMutation {
			BTreePageStore *index = nullptr;
			uint64_t index_id = 0U;
			std::optional<BTreeInitialTreeMutationPlan> plan;
		};
		struct StagedVectorBootstrapRecord {
			uint64_t node_id = 0U;
			uint64_t vector_id = 0U;
			std::vector<std::byte> raw_record;
		};
		struct StagedVectorBootstrap {
			BTreePageStore *mapping_trees = nullptr;
			VectorPageStore *raw_pages = nullptr;
			VectorManifestStore *manifests = nullptr;
			NativeGraphVectorBootstrapMutation mutation;
			size_t maximum_pages = 0U;
			std::vector<StagedVectorBootstrapRecord> records;
		};
		struct StagedVectorDeltaRecord {
			NativeVectorMutationKind kind = NativeVectorMutationKind::NVMK_ERASE;
			uint64_t node_id = 0U;
			uint64_t vector_id = 0U;
			std::vector<std::byte> raw_record;
		};
		struct StagedVectorDeltaRawPage {
			NativeGraphVectorDeltaRawPageMutation mutation;
			// The COW-tail image is built while the transaction holds its snapshot.
			// Appended/fresh pages are constructed after metadata assigns PageIds.
			std::optional<VectorPage> tail_image;
			std::vector<size_t> record_ordinals;
		};
		struct StagedVectorDelta {
			BTreePageStore *mapping_trees = nullptr;
			VectorPageStore *raw_pages = nullptr;
			VectorManifestStore *manifests = nullptr;
			NativeGraphVectorDeltaMutation mutation;
		VectorManifest source_manifest;
		size_t maximum_raw_pages = 0U;
		std::vector<StagedVectorDeltaRecord> records;
		std::vector<StagedVectorDeltaRawPage> raw_page_images;
	};

		NativeGraphWriteTransaction(std::shared_ptr<NativeGraphWritePlannerContext> context,
									MetadataSnapshotPin snapshot);
		void requireActive(const char *operation) const;
		[[nodiscard]] NodeChange &mutableNode(uint64_t nodeId);
		[[nodiscard]] EdgeChange &mutableEdge(uint64_t edgeId);
		void stageGlobalAdjacencyMutation(GlobalAdjacencyDirection direction, uint64_t nodeId, uint64_t edgeId,
									  BTreeScalarIndexOperationKind kind);
		[[nodiscard]] std::vector<BTreeScalarIndexOperation> canonicalGlobalAdjacencyOperations() const;
		void prepareGlobalAdjacencyMutationPlan();
		// Replays a bounded adjacency-only final state after validating every
		// authoritative source record against the current snapshot.
		[[nodiscard]] bool rebaseAdjacencyOnly(MetadataSnapshotPin &current);
		[[nodiscard]] bool hasRemainingIncidentEdge(uint64_t nodeId) const;
		void prepareScalarIndexMutationPlans();

		std::shared_ptr<NativeGraphWritePlannerContext> context_;
		MetadataSnapshotPin snapshot_;
		NativeGraphWriteTransactionState state_ = NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK;
		uint64_t next_vector_id_ = 0U;
		std::vector<NativeGraphLogicalIdLeaseRange> node_id_ranges_;
		std::vector<NativeGraphLogicalIdLeaseRange> edge_id_ranges_;
		std::map<uint64_t, NodeChange> nodes_;
		std::map<uint64_t, EdgeChange> edges_;
		// Ordinary graph adjacency has one reserved key-only B+Tree. Its logical
		// operations are planned with the Edge Entity mutations and published in
		// the same metadata generation.
		std::vector<BTreeScalarIndexOperation> global_adjacency_operations_;
		std::optional<BTreeStructuralMutationPlan> global_adjacency_plan_;
		std::optional<BTreeTreeRetirementPlan> global_adjacency_retirement_plan_;
		std::optional<BTreeInitialTreeMutationPlan> global_adjacency_initial_plan_;
		std::map<uint64_t, PropertyCollectionRoot> properties_;
		std::vector<StagedScalarIndexMutation> scalar_index_mutations_;
		std::vector<StagedInitialScalarIndexMutation> initial_scalar_index_mutations_;
		std::vector<StagedVectorBootstrap> vector_bootstraps_;
		std::vector<StagedVectorDelta> vector_deltas_;
	};

	class NativeGraphWritePlanner final {
	public:
		NativeGraphWritePlanner(RelocatableMetadataStore &metadata, EntityPageStore &entities,
								EntityLabelSetStore &labels, PropertyCollectionRootStore &properties, BTreePageStore &indexes,
								NativeGraphWritePlannerOptions options = {});
		[[nodiscard]] NativeGraphWriteTransaction beginTransaction();
		// Logical-transaction overlays acquire durable identities before exposing a
		// provisional result. The returned ranges are later adopted verbatim by the
		// graph writer, so another planner cannot steal or reorder these IDs.
		[[nodiscard]] NativeGraphLogicalIdReservation reserveLogicalIds(uint64_t nodeCount, uint64_t edgeCount);

	private:
		std::shared_ptr<NativeGraphWritePlannerContext> context_;
	};

} // namespace graph::storage
