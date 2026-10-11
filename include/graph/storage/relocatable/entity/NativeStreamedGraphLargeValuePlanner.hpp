/**
 * @file NativeStreamedGraphLargeValuePlanner.hpp
 * @brief Bounded LargeValue planning for streamed graph Property roots.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "graph/storage/relocatable/entity/NativeStreamedGraphPropertyPageMaterializer.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"

namespace graph::storage {

	struct NativeStreamedGraphLargeValuePlanningOptions {
		LargeValueCompressionOptions compression{};
		LargeValueExtentClass extent_class = LargeValueExtentClass::LVEC_32K;
	};

	// The first scan contains only scalar totals. It deliberately holds neither
	// root bytes nor one entry per overflow, so final placement can reserve the
	// exact durable ranges without turning a bulk transaction into an import-sized
	// in-memory LargeValue plan.
	struct NativeStreamedGraphLargeValuePlan {
		NativeStreamedGraphInlinePropertyPageCount property_pages;
		// One compact Property reference is required for each overflowing root.
		// Deduplication can make fewer immutable values materialize than references.
		size_t reference_count = 0U;
		size_t value_count = 0U;
		size_t data_page_count = 0U;
		size_t manifest_page_count = 0U;
	};

	// Valid only for the duration of the Property-page write pass. A final
	// metadata coordinator consumes one item at a time, writes its immutable
	// frames, and then allows the returned compact reference into its Property
	// page. The bytes are the canonical encoded PropertyCollectionRoot.
	struct NativeStreamedGraphLargeValueItem {
		EntityPropertyLargeValueReference stored_reference;
		std::vector<std::byte> logical_bytes;
		size_t planned_data_page_count = 0U;
		size_t planned_manifest_page_count = 0U;
	};

	using NativeStreamedGraphLargeValueItemVisitor = std::function<void(NativeStreamedGraphLargeValueItem &&)>;
	// A private sorted run maps each provisional overflow value ID to its
	// canonical immutable value ID. It is reopened for every sealed-source
	// replay, so no transaction-sized lookup table reaches the final publisher.
	using NativeStreamedGraphLargeValueReferenceMapSource = OrderedBTreeEntrySource;

	class NativeStreamedGraphLargeValuePlanner final {
	public:
		[[nodiscard]] static NativeStreamedGraphLargeValuePlan
		measure(const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
				const NativeStreamedGraphLargeValuePlanningOptions &options = {});
		// Recomputes only the immutable-value shape after a content-sorted private
		// map has selected one canonical ID for each equal payload group.
		[[nodiscard]] static NativeStreamedGraphLargeValuePlan
		deriveCanonicalPlan(const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
							LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &plan,
							const NativeStreamedGraphLargeValuePlanningOptions &options,
							const NativeStreamedGraphLargeValueReferenceMapSource &canonicalReferences);
		// `firstValueId` must be the writer-fenced next durable LargeValue ID. The
		// returned encoder assigns subsequent IDs in canonical Edge-root order and
		// rejects a source that diverges from the measured overflow cardinality.
		[[nodiscard]] static NativeStreamedGraphLargePropertyRootEncoder
		makeRootEncoder(const LargeValueStore &store, LargeValueId firstValueId,
						const NativeStreamedGraphLargeValuePlan &plan,
						const NativeStreamedGraphLargeValuePlanningOptions &options,
						const NativeStreamedGraphLargeValueItemVisitor &visit,
						const NativeStreamedGraphLargeValueReferenceMapSource &canonicalReferences = {});
		// Replays the sealed source for the final Property-page pass. Oversized
		// roots are replaced with their deterministic compact references, but their
		// bytes are deliberately discarded here. This lets the final coordinator
		// publish Property pages before a later PageId-ordered LargeValue pass,
		// without retaining one item per overflow root.
		static void materializePropertyPages(const NativeStreamedGraphDeferredPublication &publication,
											 const LargeValueStore &store, LargeValueId firstValueId,
											 const NativeStreamedGraphLargeValuePlan &plan,
											 const NativeStreamedGraphLargeValuePlanningOptions &options,
											 const NativeStreamedGraphPropertyPageAssignmentSource &assign,
											 const NativeStreamedGraphPropertyPageVisitor &visitPage,
											 const NativeStreamedGraphLargeValueReferenceMapSource &canonicalReferences = {});
		// Replays only the overflow roots from the same sealed source. It emits one
		// materialization item at a time in the identical canonical Edge-root
		// order used by materializePropertyPages(), and verifies all measured
		// scalar totals before returning. The consumer must materialize and release
		// each item before this callback returns.
		static void materializeLargeValues(const NativeStreamedGraphDeferredPublication &publication,
										   const LargeValueStore &store, LargeValueId firstValueId,
										   const NativeStreamedGraphLargeValuePlan &plan,
										   const NativeStreamedGraphLargeValuePlanningOptions &options,
										   const NativeStreamedGraphLargeValueItemVisitor &visitItem,
										   const NativeStreamedGraphLargeValueReferenceMapSource &canonicalReferences = {});
	};

} // namespace graph::storage
