/**
 * @file NativeBulkEdgePrivateRunMaterializer.hpp
 * @brief Persists one bounded streamed Edge-derived run-reduction level.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRunReducer.hpp"

namespace graph::storage {

	// The caller owns candidate ordinal assignment. This materializer receives no
	// Catalog, allocator, physical-slot, or public-metadata capability.
	using NativeBulkEdgePrivateCandidateAllocator = std::function<NativeGraphPrivateSegmentMaterialization()>;
	using NativeBulkEdgePrivateCandidateWriter =
			std::function<void(const NativeGraphPrivateSegmentMaterialization &, std::vector<std::byte>)>;
	using NativeBulkEdgePrivateRunRootVisitor = std::function<void(NativeBulkRunFragmentLocation)>;
	using NativeBulkEdgePrivateRunFragmentVisitor = std::function<void(NativeBulkRunFragment &&)>;

	struct NativeBulkEdgePrivateRunMaterializationOptions {
		size_t maximum_open_runs = 0U;
		size_t maximum_fragment_bytes = 0U;
		size_t maximum_entries = 0U;
		uint16_t maximum_manifest_depth = 0U;
		NativeBulkEdgeRunDuplicatePolicy duplicate_policy = NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT;
	};

	// Converts one reducer level into immutable private fragments and one root
	// manifest for every logical output run. The working set is one reducer group
	// plus one manifest fan-out buffer per manifest level; it never retains all
	// output fragment identities. The caller may spool root manifests through its
	// own bounded private run-set representation for a later reduction level.
	class NativeBulkEdgePrivateRunMaterializer final {
	public:
		// Seals one already-sorted initial run as a private fragment plus its
		// manifest. It avoids manufacturing an in-memory run-source collection
		// merely to enter the bounded merge pipeline.
		[[nodiscard]] static NativeBulkRunFragmentLocation materializeInitialFragment(
				NativeBulkRunFragment fragment, uint16_t maximumManifestDepth,
				const NativeBulkEdgePrivateCandidateAllocator &allocateCandidate,
				const NativeBulkEdgePrivateCandidateWriter &writeCandidate);

		static void materializeOneLevel(const NativeBulkEdgeRunSource &input,
										const NativeBulkEdgePrivateRunMaterializationOptions &options,
										const NativeBulkEdgePrivateCandidateAllocator &allocateCandidate,
										const NativeBulkEdgePrivateCandidateWriter &writeCandidate,
										const NativeBulkEdgePrivateRunRootVisitor &visitCompletedRun);
	};

} // namespace graph::storage
