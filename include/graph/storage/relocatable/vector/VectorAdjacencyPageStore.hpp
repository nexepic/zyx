/**
 * @file VectorAdjacencyPageStore.hpp
 * @brief Staged immutable adjacency-page materialization for relocatable vectors.
 */

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacency.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"

namespace graph::storage {

	// Derived adjacency generations use a disjoint owner namespace so their
	// immutable pages can be validated and reclaimed independently.
	constexpr uint64_t RSS_DERIVED_ADJACENCY_OWNER_MASK = UINT64_C(1) << 63U;

	struct VectorAdjacencyMaterializationRecord {
		uint64_t vector_id = 0;
		std::vector<uint64_t> neighbors;

		bool operator==(const VectorAdjacencyMaterializationRecord &) const = default;
	};

	struct VectorAdjacencyMaterializationOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t maximum_records = 0;
		size_t maximum_pages = 0;
	};

	struct VectorAdjacencyMaterializationPage {
		PageId page_id{};
		uint32_t record_count = 0;

		constexpr bool operator==(const VectorAdjacencyMaterializationPage &) const = default;
	};

struct VectorAdjacencyMaterializationResult {
		Superblock superblock;
		uint64_t record_count = 0;
		std::vector<VectorAdjacencyMaterializationPage> pages;
		// This sorted output must be bound through VectorAdjacencyMapping and a
		// Manifest replacement before a derived generation is query-visible.
		std::vector<VectorAdjacencyPageMapping> mappings;

	bool operator==(const VectorAdjacencyMaterializationResult &) const = default;
};

// A read-only observation of one page in the derived adjacency namespace.
struct DerivedVectorAdjacencyPage {
	PageLocation location;
	uint64_t index_id = 0;
	LogSequenceNumber owner_lsn{};

	constexpr bool operator==(const DerivedVectorAdjacencyPage &) const = default;
};

	class RelocatableMetadataStore;

	class VectorAdjacencyPageStore final {
	public:
		VectorAdjacencyPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata);
		VectorAdjacencyPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
								 SharedPagePoolOpenMode openMode);
		[[nodiscard]] static uint64_t derivedOwnerId(uint64_t indexId);
		[[nodiscard]] static std::vector<VectorAdjacencyMaterializationPoolRequest>
		planNewPoolRequests(uint64_t indexId, std::span<const VectorAdjacencyMaterializationRecord> records,
							const VectorAdjacencyMaterializationOptions &options, LogSequenceNumber checkpointLsn);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}

		// Stages immutable adjacency records. A Manifest-published mapping tree
		// binds this materialization to the visible derived generation.
		[[nodiscard]] VectorAdjacencyMaterializationResult
		materialize(uint64_t indexId, std::span<const VectorAdjacencyMaterializationRecord> records,
					const VectorAdjacencyMaterializationOptions &options, LogSequenceNumber checkpointLsn,
					const MetadataPublishOptions &publishOptions = {});
		// Prepares immutable pages after an enclosing compound metadata writer has
		// assigned their pools and PageIds. No Page Directory or Manifest state is
		// published here, allowing a base rebuild to switch PQ, adjacency, and its
		// delta root in one generation.
		[[nodiscard]] VectorAdjacencyMaterializationResult
		prepareNewPoolPages(uint64_t indexId, std::span<const VectorAdjacencyMaterializationRecord> records,
							const VectorAdjacencyMaterializationOptions &options,
							std::span<VectorAdjacencyMaterializationPoolBatch> batches,
							LogSequenceNumber checkpointLsn,
							const CancellationCheckpoint *cancellation = nullptr) const;
		// Retires derived adjacency source frames before an enclosing compound
		// publication removes their Page Directory references.
		void prepareDerivedRetirements(std::span<const ExistingPoolPageRetirementBatch> batches,
								 LogSequenceNumber checkpointLsn,
								 const CancellationCheckpoint *cancellation = nullptr) const;
		[[nodiscard]] SlottedPage readPage(const MetadataSnapshotPin &snapshot, PageId pageId) const;
		// Returns no value for another valid typed page in the shared pool.
		// Malformed or cross-namespace pages fail closed.
		[[nodiscard]] std::optional<DerivedVectorAdjacencyPage>
		inspectDerivedPage(const MetadataSnapshotPin &snapshot, PageId pageId) const;
		// Derived pages are immutable and records are written in strictly increasing
		// Vector ID order, allowing callers to validate a mapping hit without the
		// mutable global adjacency directory.
		[[nodiscard]] VectorAdjacencyRecord readRecord(const MetadataSnapshotPin &snapshot, PageId pageId,
															   uint64_t vectorId) const;
		// Retires only pages whose complete derived owner tuple was observed in one
		// metadata generation. The metadata writer fences that generation again
		// before removing Page Directory reachability.
		[[nodiscard]] Superblock retireDerivedPages(Generation expectedMetadataGeneration,
																std::span<const DerivedVectorAdjacencyPage> pages,
																LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &publishOptions = {});
		void reclaimRetiredFrames(SegmentId segmentId) const;

	private:
		SharedPagePoolAllocator allocator_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
