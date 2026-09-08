/**
 * @file NativeStreamedGraphEntityPageMaterializer.cpp
 * @brief Bounded Edge Entity-page images for streamed graph publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphEntityPageMaterializer.hpp"

#include "graph/debug/PerfTrace.hpp"

namespace graph::storage {

	void NativeStreamedGraphEntityPageMaterializer::materialize(
			const NativeStreamedGraphDeferredPublication &publication,
			const NativeStreamedGraphEntityPageAssignmentSource &assign,
			const NativeStreamedGraphEntityPageVisitor &visit) {
		if (!assign || !visit) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
							  "native streamed Edge Entity-page materialization requires a complete bounded publication");
		}
		size_t pageOrdinal = 0U;
		debug::ScopedPerfTimer sourceTimer("native_graph.streamed.final.entity_page_source");
		publication.visitEntityPages([&](NativeBulkEdgeEntityPageBatch &&batch) {
			const NativeStreamedGraphEntityPageAssignment assignment = assign(pageOrdinal);
			const PageOwnerHeader &owner = assignment.owner;
			if (!owner.page_id.isValid() || owner.owner_id == 0U || owner.kind != PageOwnerKind::POK_ENTITY ||
				owner.flags != 0U || owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES ||
				!owner.generation.isValid() || !owner.committed_lsn.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native streamed Edge Entity-page assignment has an invalid metadata-owned identity");
			}
			EntityPage page = EntityPage::create(owner, EntityPageRecordKind::EPRK_EDGE);
			NativeStreamedGraphEntityPage result;
			result.owner = owner;
			result.edge_locations.reserve(batch.records.size());
			std::vector<EdgeEntityRecord> records;
			records.reserve(batch.records.size());
			for (const NativeBulkEdgeMaterializedRecord &record: batch.records) {
				records.push_back(record.entity);
			}
			const std::vector<uint16_t> slots = page.insertEdges(records);
			for (size_t index = 0U; index < batch.records.size(); ++index) {
				const uint16_t slot = slots[index];
				result.edge_locations.push_back(
						{batch.records[index].edge_id, owner.page_id, slot, page.slotGeneration(slot)});
			}
			result.payload = page.serialize();
			visit(std::move(result));
			++pageOrdinal;
		});
	}

} // namespace graph::storage
