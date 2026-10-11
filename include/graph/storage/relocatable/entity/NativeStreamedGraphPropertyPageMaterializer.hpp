/**
 * @file NativeStreamedGraphPropertyPageMaterializer.hpp
 * @brief Bounded Edge Property-page images for streamed graph publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "graph/storage/relocatable/entity/NativeStreamedGraphDeferredPublication.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace graph::storage {

	// The metadata coordinator assigns the stable PageId and Property-pool
	// identity before this page-local materializer serializes a SlottedPage.
	struct NativeStreamedGraphPropertyPageAssignment {
		PageOwnerHeader owner;
	};

	struct NativeStreamedGraphPropertyPage {
		PageOwnerHeader owner;
		std::vector<std::byte> payload;
		std::vector<EntityPropertyLocation> property_locations;
		uint32_t free_bytes = 0U;
	};

	using NativeStreamedGraphPropertyPageAssignmentSource =
			std::function<NativeStreamedGraphPropertyPageAssignment(size_t pageOrdinal)>;
	using NativeStreamedGraphPropertyPageVisitor = std::function<void(NativeStreamedGraphPropertyPage &&)>;
	// Invoked only when an encoded Property root cannot fit in a Property-page
	// slot. The callback must be deterministic across the measurement and write
	// passes, and return a compact, owner-authenticated LargeValue reference that
	// will be made reachable in the same metadata generation. Keeping this
	// hand-off root-local preserves the streamed publisher's bounded memory
	// contract: it never accumulates transaction-wide overflow roots.
	using NativeStreamedGraphLargePropertyRootEncoder =
			std::function<std::vector<std::byte>(const PropertyCollectionRoot &, std::vector<std::byte> &&)>;

	struct NativeStreamedGraphInlinePropertyPageCount {
		size_t page_count = 0U;
		size_t property_count = 0U;
	};

	// Produces exactly one SlottedPage at a time.  It deliberately excludes roots
	// requiring a LargeValue reference: the final coordinator must materialize
	// that value and its manifest in the same metadata generation before making
	// the compact Property reference visible.
	class NativeStreamedGraphPropertyPageMaterializer final {
	public:
		[[nodiscard]] static NativeStreamedGraphInlinePropertyPageCount
		measureInlinePages(const NativeStreamedGraphDeferredPublication &publication);
		[[nodiscard]] static size_t countInlinePages(const NativeStreamedGraphDeferredPublication &publication);
		static void materializeInlinePages(const NativeStreamedGraphDeferredPublication &publication,
									   const NativeStreamedGraphPropertyPageAssignmentSource &assign,
									   const NativeStreamedGraphPropertyPageVisitor &visit);
		// The final streamed publisher uses this form to replace an oversized root
		// with a same-generation LargeValue reference before inserting it. Inline
		// roots keep their original bytes and avoid any callback or allocation.
		[[nodiscard]] static NativeStreamedGraphInlinePropertyPageCount
		measurePages(const NativeStreamedGraphDeferredPublication &publication,
					 const NativeStreamedGraphLargePropertyRootEncoder &encodeLargeRoot);
		static void materializePages(const NativeStreamedGraphDeferredPublication &publication,
							 const NativeStreamedGraphPropertyPageAssignmentSource &assign,
							 const NativeStreamedGraphPropertyPageVisitor &visit,
							 const NativeStreamedGraphLargePropertyRootEncoder &encodeLargeRoot);
	};

} // namespace graph::storage
