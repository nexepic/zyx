/**
 * @file NativeBulkEdgePrivateTokenResolver.hpp
 * @brief Candidate-private bounded Token lookup for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "graph/storage/relocatable/entity/NativeBulkEdgeMaterializer.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.hpp"
#include "graph/storage/relocatable/schema/NativeBulkEdgeCatalogTokenMaterializer.hpp"

namespace graph::storage {

	// A private immutable B+Tree-like lookup structure built only inside the
	// candidate range. Its SegmentIds are never published, and the object is
	// valid only for the enclosing metadata-writer callback scope.
	class NativeBulkEdgePrivateTokenResolver final {
	public:
		[[nodiscard]] static NativeBulkEdgePrivateTokenResolver makeEmpty();
		[[nodiscard]] static size_t maximumCandidateSegments(size_t maximumTokenEntries);

		[[nodiscard]] static NativeBulkEdgePrivateTokenResolver materialize(
				const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
				const NativeBulkEdgeCatalogTokenResolutionSource &resolutions, size_t maximumTokenEntries,
				const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
				const NativeGraphPrivateSegmentFence &fence);

		[[nodiscard]] std::optional<uint64_t> find(NativeBulkEdgeTokenKind kind, std::string_view name) const;
		// Creates the only resolver closures accepted by the streamed Edge
		// materializer. The closures borrow this callback-scoped private tree and
		// therefore cannot outlive the candidate publication boundary.
		[[nodiscard]] NativeBulkEdgeMaterializerResolvers materializerResolvers() const;
		[[nodiscard]] size_t materializedSegmentCount() const { return materialized_segment_count_; }
		[[nodiscard]] size_t maximumCandidateSegmentCount() const { return candidate_range_.count; }
		[[nodiscard]] bool empty() const { return !root_.isValid(); }

	private:
		NativeBulkEdgePrivateTokenResolver(const NativeGraphPrivateSegmentSet *identities,
				NativeBulkEdgePrivateCandidateRange candidateRange, NativeGraphPrivateSegmentMaterialization root,
				size_t materializedSegmentCount, NativeGraphPrivateSegmentReader reader) :
			identities_(identities), candidate_range_(candidateRange), root_(root),
			materialized_segment_count_(materializedSegmentCount), reader_(std::move(reader)) {}

		const NativeGraphPrivateSegmentSet *identities_ = nullptr;
		NativeBulkEdgePrivateCandidateRange candidate_range_{};
		NativeGraphPrivateSegmentMaterialization root_{};
		size_t materialized_segment_count_ = 0U;
		NativeGraphPrivateSegmentReader reader_;
	};

} // namespace graph::storage
