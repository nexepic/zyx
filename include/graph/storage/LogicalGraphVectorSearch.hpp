/**
 * @file LogicalGraphVectorSearch.hpp
 * @brief Snapshot-pinned, format-independent logical vector-search boundary.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::storage {

	// Prefixes avoid Windows macro collisions. Exact mode is intended for
	// validation/calibration; ordinary query execution uses derived data when it
	// is atomically available.
	enum class LogicalGraphVectorSearchMode : uint8_t {
		LGVSM_AUTO = 0,
		LGVSM_EXACT = 1,
	};

	// All bounds are mandatory so a procedure call cannot turn a logical search
	// into an unbounded mapping walk or raw-page read.
	struct LogicalGraphVectorSearchRequest {
		std::string index_name;
		std::vector<float> query_vector;
		size_t top_k = 0U;
		LogicalGraphVectorSearchMode mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
		// Metadata/data bounds are corruption and resource-safety ceilings. The
		// candidate budget is instead a normal ANN termination condition.
		size_t maximum_mapping_entries = 0U;
		size_t maximum_raw_vector_reads = 0U;
		size_t candidate_search_budget = 0U;
		size_t candidate_refinement_budget = 0U;
		// Non-owning and valid only for this synchronous search call. The
		// checkpoint keeps storage independent from QueryGuard while allowing
		// long bounded scans to observe cancellation promptly.
		const CancellationCheckpoint *cancellation = nullptr;

		bool operator==(const LogicalGraphVectorSearchRequest &other) const {
			return index_name == other.index_name && query_vector == other.query_vector && top_k == other.top_k &&
				   mode == other.mode && maximum_mapping_entries == other.maximum_mapping_entries &&
				   maximum_raw_vector_reads == other.maximum_raw_vector_reads &&
				   candidate_search_budget == other.candidate_search_budget &&
				   candidate_refinement_budget == other.candidate_refinement_budget;
		}
	};

	// Scores use the index's stable distance ordering: lower is better. This
	// matches the existing vector procedure and avoids exposing storage metrics
	// or physical vector identities at the query boundary.
	struct LogicalGraphVectorSearchHit {
		uint64_t node_id = 0U;
		float score = 0.0F;

		bool operator==(const LogicalGraphVectorSearchHit &) const = default;
	};

	// Transient structural work counters support reproducible calibration without
	// making host-dependent latency part of the storage contract.
	struct LogicalGraphVectorSearchStats {
		size_t scanned_mapping_entries = 0U;
		size_t scanned_delta_entries = 0U;
		size_t semantic_mapping_lookups = 0U;
		size_t derived_mapping_lookups = 0U;
		size_t candidate_discoveries = 0U;
		size_t candidate_expansions = 0U;
		size_t candidate_frontier_peak = 0U;
		size_t adjacency_record_reads = 0U;
		size_t pq_record_reads = 0U;
		size_t raw_vector_reads = 0U;
		size_t exact_overlay_candidate_scans = 0U;
		bool used_adjacency = false;
		bool used_pq = false;
		bool used_delta = false;
		bool used_exact_overlay = false;

		bool operator==(const LogicalGraphVectorSearchStats &) const = default;
	};

	struct LogicalGraphVectorSearchResult {
		std::vector<LogicalGraphVectorSearchHit> hits;
		LogicalGraphVectorSearchStats stats;

		bool operator==(const LogicalGraphVectorSearchResult &) const = default;
	};

	// A snapshot exposes only the logical identity needed to evaluate a
	// transaction-local exact fallback. It deliberately omits physical index,
	// page, and codec identities.
	struct LogicalGraphVectorIndexDescriptor {
		uint64_t index_id = 0U;
		std::string label;
		std::string property_key;
		uint32_t dimensions = 0U;
		std::string metric;

		bool operator==(const LogicalGraphVectorIndexDescriptor &) const = default;
	};

	// Search and graph hydration share one snapshot. A returned Node ID can
	// therefore be materialized through the inherited logical graph reader
	// without observing a later graph/vector generation.
	class LogicalGraphVectorSearchSnapshot : public LogicalGraphReadSnapshot {
	public:
		~LogicalGraphVectorSearchSnapshot() override = default;

		[[nodiscard]] virtual LogicalGraphVectorSearchResult
		searchLogicalVector(const LogicalGraphVectorSearchRequest &request) const = 0;
		// A write transaction may have a graph overlay that a pinned derived index
		// cannot represent. Implementations opt into this pair to let the overlay
		// scan its final Node state exactly, while retaining metric/codec semantics
		// at the owner of the pinned Vector generation. A candidate scorer accepts
		// only a descriptor returned by this same immutable snapshot.
		[[nodiscard]] virtual std::optional<LogicalGraphVectorIndexDescriptor>
		describeLogicalVectorIndex(const LogicalGraphVectorSearchRequest & /*request*/) const {
			return std::nullopt;
		}
		// A scorer may opt into a narrow candidate shape. The default preserves
		// complete candidate semantics for providers requiring additional fields.
		[[nodiscard]] virtual EntityReadShape
		exactOverlayCandidateReadShape(const LogicalGraphVectorIndexDescriptor & /*index*/) const {
			return {.include_labels = true, .all_properties = true};
		}
		[[nodiscard]] virtual std::optional<LogicalGraphVectorSearchHit>
		scoreLogicalVectorExactOverlayCandidate(const LogicalGraphVectorSearchRequest & /*request*/,
												   const LogicalGraphVectorIndexDescriptor & /*index*/,
												   const LogicalGraphNode & /*node*/) const {
			throw std::logic_error("logical Vector snapshot does not support exact overlay candidates");
		}
	};

	// Kept separate from LogicalGraphReadBackend: backends must explicitly opt
	// into vector-search correctness rather than inheriting it from graph reads.
	class LogicalGraphVectorSearchBackend {
	public:
		virtual ~LogicalGraphVectorSearchBackend() = default;

		[[nodiscard]] virtual std::unique_ptr<LogicalGraphVectorSearchSnapshot>
		openLogicalGraphVectorSearchSnapshot() const = 0;
	};

	// A logical transaction that pins a derived Vector generation exposes it
	// through this opt-in capability. The transaction keeps ownership, so an
	// overlay cannot accidentally reopen a later autocommit generation.
	class LogicalGraphTransactionVectorSnapshotProvider {
	public:
		virtual ~LogicalGraphTransactionVectorSnapshotProvider() = default;

		[[nodiscard]] virtual LogicalGraphVectorSearchSnapshot &
		pinnedLogicalGraphVectorSearchSnapshot() = 0;
	};

	// Vector training changes only derived acceleration data. Keep its write
	// boundary separate from search so readers cannot accidentally acquire a
	// maintenance capability merely by supporting snapshot-pinned search.
	struct LogicalGraphVectorTrainingRequest {
		std::string index_name;
		size_t maximum_records = 0U;
		size_t maximum_pages = 0U;
		// Non-owning and valid only for this synchronous maintenance call. Training
		// checks it while it is still assembling an unpublished derived generation.
		const CancellationCheckpoint *cancellation = nullptr;

		bool operator==(const LogicalGraphVectorTrainingRequest &other) const {
			return index_name == other.index_name && maximum_records == other.maximum_records &&
				   maximum_pages == other.maximum_pages;
		}
	};

	struct LogicalGraphVectorTrainingResult {
		size_t vector_count = 0U;
		bool trained = false;
		bool reused_existing_generation = false;

		bool operator==(const LogicalGraphVectorTrainingResult &) const = default;
	};

	// This is intentionally narrower than LogicalGraphVectorTrainingResult. A
	// procedure must materialize its YIELD/RETURN pipeline before publishing a
	// derived generation, but only the public status is needed at that point.
	// The prepared operation keeps the source generation fenced until commit().
	struct LogicalGraphVectorTrainingPreview {
		bool will_train = false;

		bool operator==(const LogicalGraphVectorTrainingPreview &) const = default;
	};

	class PreparedLogicalGraphVectorTraining {
	public:
		virtual ~PreparedLogicalGraphVectorTraining() = default;

		[[nodiscard]] virtual LogicalGraphVectorTrainingPreview preview() const noexcept = 0;
		// Commits one complete derived generation. Destroying an uncommitted
		// operation abandons the preparation without publishing anything.
		[[nodiscard]] virtual LogicalGraphVectorTrainingResult commit() = 0;
	};

	class LogicalGraphVectorMaintenanceBackend {
	public:
		virtual ~LogicalGraphVectorMaintenanceBackend() = default;

		// Preparation has no externally visible side effect. It returns a fenced
		// operation so a caller can validate and materialize its result pipeline
		// before crossing the one durable publication boundary.
		[[nodiscard]] virtual std::unique_ptr<PreparedLogicalGraphVectorTraining>
		prepareLogicalVectorTraining(const LogicalGraphVectorTrainingRequest &request) = 0;

		// Convenience entry point for internal maintenance callers that do not
		// have a result pipeline. Implementations publish a complete derived
		// generation atomically or leave the prior visible generation unchanged.
		[[nodiscard]] virtual LogicalGraphVectorTrainingResult
		trainLogicalVector(const LogicalGraphVectorTrainingRequest &request) {
			std::unique_ptr<PreparedLogicalGraphVectorTraining> prepared = prepareLogicalVectorTraining(request);
			if (!prepared) {
				throw std::logic_error("logical Vector maintenance backend returned no prepared training operation");
			}
			return prepared->commit();
		}
	};

} // namespace graph::storage
