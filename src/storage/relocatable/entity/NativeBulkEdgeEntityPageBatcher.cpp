/**
 * @file NativeBulkEdgeEntityPageBatcher.cpp
 * @brief Page-local batching for streamed native Edge materialization.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeBulkEdgeEntityPageBatcher.hpp"

#include <utility>

namespace graph::storage {

	void NativeBulkEdgeEntityPageBatcher::materializePages(
			const NativeBulkEdgeRecordSource &source,
			std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges, size_t maximumRecords,
			const NativeBulkEdgeMaterializerResolvers &resolvers,
			const NativeBulkEdgeEntityPageBatchVisitor &visitor) {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
							  "native bulk Edge Entity-page batcher requires a batch visitor");
		}
		const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
		if (capacity == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "native bulk Edge Entity-page batcher found a zero Edge-page capacity");
		}

		NativeBulkEdgeEntityPageBatch current;
		current.records.reserve(capacity);
		NativeBulkEdgeMaterializer::materialize(
				source, edgeIdRanges, maximumRecords, resolvers,
				[&current, capacity, &visitor](NativeBulkEdgeMaterializedRecord &&record) {
					current.records.push_back(std::move(record));
					if (current.records.size() == capacity) {
						visitor(std::move(current));
						current = {};
						current.records.reserve(capacity);
					}
				});
		if (!current.records.empty()) {
			visitor(std::move(current));
		}
	}

} // namespace graph::storage
