/**
 * @file NativeStreamedGraphDeferredPublication.hpp
 * @brief Callback-scoped typed page sources for one streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/entity/NativeBulkEdgeEntityPageBatcher.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationPlan.hpp"

namespace graph::storage {

	using NativeStreamedGraphPropertyRootVisitor = std::function<void(PropertyCollectionRoot &&)>;

	// This is an immutable, callback-scoped bridge between private preparation
	// and final graph-page materialization. It deliberately owns no record list,
	// PageId, SegmentId, physical slot, Catalog mutation, or metadata handle. A
	// caller can only reopen the sealed candidate source and consume bounded
	// Entity-page batches or owner-keyed Property roots in canonical Edge-ID
	// order. The metadata coordinator remains the sole owner of final identity
	// allocation and publication.
	class NativeStreamedGraphDeferredPublication final {
	public:
		[[nodiscard]] static NativeStreamedGraphDeferredPublication create(
				const NativeBulkEdgeRecordSource &source,
				std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
				const NativeStreamedGraphPublicationPlan &publicationPlan,
				const NativeBulkEdgeMaterializerResolvers &resolvers);

		[[nodiscard]] uint64_t recordCount() const { return record_count_; }
		[[nodiscard]] uint64_t firstEdgeId() const { return first_edge_id_; }
		[[nodiscard]] uint64_t lastEdgeId() const { return last_edge_id_; }
		[[nodiscard]] size_t entityPageCount() const { return entity_page_count_; }

		// Each call reopens the sealed source. The visitor receives at most one
		// codec-sized Entity batch, and may not retain it after returning.
		void visitEntityPages(const NativeBulkEdgeEntityPageBatchVisitor &visitor) const;
		// Property roots follow the same canonical Edge-ID order but are streamed
		// one at a time, allowing the later Property planner to remain page-local.
		void visitPropertyRoots(const NativeStreamedGraphPropertyRootVisitor &visitor) const;

	private:
		NativeStreamedGraphDeferredPublication(
				NativeBulkEdgeRecordSource source, std::vector<NativeGraphLogicalIdLeaseRange> edgeIdRanges,
				NativeBulkEdgeMaterializerResolvers resolvers, uint64_t recordCount, uint64_t firstEdgeId,
				uint64_t lastEdgeId, size_t entityPageCount) :
			source_(std::move(source)), edge_id_ranges_(std::move(edgeIdRanges)), resolvers_(std::move(resolvers)),
			record_count_(recordCount), first_edge_id_(firstEdgeId), last_edge_id_(lastEdgeId),
			entity_page_count_(entityPageCount) {}

		NativeBulkEdgeRecordSource source_;
		std::vector<NativeGraphLogicalIdLeaseRange> edge_id_ranges_;
		NativeBulkEdgeMaterializerResolvers resolvers_;
		uint64_t record_count_ = 0U;
		uint64_t first_edge_id_ = 0U;
		uint64_t last_edge_id_ = 0U;
		size_t entity_page_count_ = 0U;
	};

} // namespace graph::storage
