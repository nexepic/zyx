/**
 * @file NativeStreamedGraphEntityPageMaterializer.cpp
 * @brief Bounded Edge Entity-page images for streamed graph publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphEntityPageMaterializer.hpp"

#include <limits>

#include "graph/debug/PerfTrace.hpp"

namespace graph::storage {

	void NativeStreamedGraphEntityPageMaterializer::materialize(
			const NativeStreamedGraphDeferredPublication &publication,
			const NativeStreamedGraphEntityPageAssignmentSource &assign,
			const NativeStreamedGraphEntityPageVisitor &visit) {
		if (!assign || !visit || publication.recordCount() == 0U || publication.entityPageCount() == 0U) { // ZYX_COV_EXCL_LINE: DeferredPublication::create rejects zero-shaped publications before materialization.
			throw FormatException(FormatError::FERR_INVALID_HEADER,
							  "native streamed Edge Entity-page materialization requires a complete bounded publication");
		}
		size_t pageOrdinal = 0U;
		uint64_t recordCount = 0U;
		debug::ScopedPerfTimer sourceTimer("native_graph.streamed.final.entity_page_source");
		publication.visitEntityPages([&](NativeBulkEdgeEntityPageBatch &&batch) {
			if (pageOrdinal == publication.entityPageCount() || batch.records.empty()) { // ZYX_COV_EXCL_LINE: the bounded page batcher emits exactly the validated nonempty page sequence.
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native streamed Edge Entity-page source emitted an invalid page batch");
			}
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
			uint64_t previousEdgeId = 0U;
			for (const NativeBulkEdgeMaterializedRecord &record: batch.records) {
				if (record.edge_id == 0U || record.edge_id <= previousEdgeId || // ZYX_COV_EXCL_LINE: source replay validation rejects non-canonical IDs before the entity-page callback.
					recordCount == std::numeric_limits<uint64_t>::max()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native streamed Edge Entity-page source lost canonical Edge-ID ordering");
				}
				records.push_back(record.entity);
				previousEdgeId = record.edge_id;
				++recordCount;
			}
			const std::vector<uint16_t> slots = page.insertEdges(records);
			if (slots.size() != batch.records.size()) { // ZYX_COV_EXCL_LINE: EntityPage::insertEdges returns one slot for every validated edge record.
				throw FormatException(FormatError::FERR_INVALID_STATE,
							  "native streamed Edge Entity-page batch lost one assigned fixed-record slot");
			}
			for (size_t index = 0U; index < batch.records.size(); ++index) {
				const uint16_t slot = slots[index];
				result.edge_locations.push_back(
						{batch.records[index].edge_id, owner.page_id, slot, page.slotGeneration(slot)});
			}
			result.payload = page.serialize();
			visit(std::move(result));
			++pageOrdinal;
		});
		if (pageOrdinal != publication.entityPageCount() || recordCount != publication.recordCount()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "native streamed Edge Entity-page source diverged from its immutable publication shape");
		}
	}

} // namespace graph::storage
