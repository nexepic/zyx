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
#include <string>
#include <vector>

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

		bool operator==(const LogicalGraphVectorSearchRequest &) const = default;
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
		bool used_adjacency = false;
		bool used_pq = false;
		bool used_delta = false;

		bool operator==(const LogicalGraphVectorSearchStats &) const = default;
	};

	struct LogicalGraphVectorSearchResult {
		std::vector<LogicalGraphVectorSearchHit> hits;
		LogicalGraphVectorSearchStats stats;

		bool operator==(const LogicalGraphVectorSearchResult &) const = default;
	};

	// Search and graph hydration share one snapshot. A returned Node ID can
	// therefore be materialized through the inherited logical graph reader
	// without observing a later graph/vector generation.
	class LogicalGraphVectorSearchSnapshot : public LogicalGraphReadSnapshot {
	public:
		~LogicalGraphVectorSearchSnapshot() override = default;

		[[nodiscard]] virtual LogicalGraphVectorSearchResult
		searchLogicalVector(const LogicalGraphVectorSearchRequest &request) const = 0;
	};

	// Kept separate from LogicalGraphReadBackend: backends must explicitly opt
	// into vector-search correctness rather than inheriting it from graph reads.
	class LogicalGraphVectorSearchBackend {
	public:
		virtual ~LogicalGraphVectorSearchBackend() = default;

		[[nodiscard]] virtual std::unique_ptr<LogicalGraphVectorSearchSnapshot>
		openLogicalGraphVectorSearchSnapshot() const = 0;
	};

	// Vector training changes only derived acceleration data. Keep its write
	// boundary separate from search so readers cannot accidentally acquire a
	// maintenance capability merely by supporting snapshot-pinned search.
	struct LogicalGraphVectorTrainingRequest {
		std::string index_name;
		size_t maximum_records = 0U;
		size_t maximum_pages = 0U;

		bool operator==(const LogicalGraphVectorTrainingRequest &) const = default;
	};

	struct LogicalGraphVectorTrainingResult {
		size_t vector_count = 0U;
		bool trained = false;
		bool reused_existing_generation = false;

		bool operator==(const LogicalGraphVectorTrainingResult &) const = default;
	};

	class LogicalGraphVectorMaintenanceBackend {
	public:
		virtual ~LogicalGraphVectorMaintenanceBackend() = default;

		// Implementations must publish a complete derived generation atomically or
		// leave the prior visible generation unchanged.
		[[nodiscard]] virtual LogicalGraphVectorTrainingResult
		trainLogicalVector(const LogicalGraphVectorTrainingRequest &request) = 0;
	};

} // namespace graph::storage
