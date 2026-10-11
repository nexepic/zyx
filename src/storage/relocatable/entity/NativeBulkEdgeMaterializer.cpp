/**
 * @file NativeBulkEdgeMaterializer.cpp
 * @brief Bounded typed conversion of sealed streamed Edge scratch records.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeBulkEdgeMaterializer.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "graph/core/Edge.hpp"
namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool isLeased(std::span<const NativeGraphLogicalIdLeaseRange> ranges, uint64_t edgeId) {
			return std::ranges::any_of(ranges,
								 [edgeId](const NativeGraphLogicalIdLeaseRange &range) { return range.contains(edgeId); });
		}

	} // namespace

	void NativeBulkEdgeMaterializer::materialize(const NativeBulkEdgeRecordSource &source,
											  std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
											  size_t maximumRecords,
											  const NativeBulkEdgeMaterializerResolvers &resolvers,
											  const NativeBulkEdgeMaterializedRecordVisitor &visitor) {
		if (!source || edgeIdRanges.empty() || maximumRecords == 0U || !resolvers.relationship_type_token ||
			!resolvers.property_key_token || !visitor) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge materializer requires a source, exact lease, finite bound, resolvers, and visitor");
		}
		uint64_t previousLeaseEnd = 0U;
		for (const NativeGraphLogicalIdLeaseRange &range: edgeIdRanges) {
			if (range.first_id == 0U || range.count == 0U || range.count - 1U >
																			std::numeric_limits<uint64_t>::max() - range.first_id ||
				(previousLeaseEnd != 0U && range.first_id <= previousLeaseEnd)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge materializer requires ordered nonoverlapping logical-ID leases");
			}
			previousLeaseEnd = range.first_id + range.count - 1U;
		}

		uint64_t previousEdgeId = 0U;
		size_t materialized = 0U;
		NativeBulkEdgeRecordCursor cursor = source();
		while (const std::optional<NativeBulkEdgeRecord> record = cursor()) {
			// Source construction normally decoded this record from a sealed run. The
			// materializer repeats canonical validation because it is also the last
			// typed boundary before graph-visible pages are prepared.
			static_cast<void>(NativeBulkEdgeRecordCodec::encodedSize(*record));
			if (++materialized > maximumRecords || record->edge_id == 0U || record->edge_id <= previousEdgeId ||
				!isLeased(edgeIdRanges, record->edge_id)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge materializer found unordered, over-bound, or unleased scratch records");
			}
			previousEdgeId = record->edge_id;

			uint64_t typeId = RSS_UNTYPED_RELATIONSHIP_TYPE_TOKEN;
			if (!record->type.empty()) {
				const std::optional<uint64_t> resolved = resolvers.relationship_type_token(record->type);
				if (!resolved.has_value() || *resolved == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge materializer could not resolve a relationship-type Token");
				}
				typeId = *resolved;
			}

			NativeBulkEdgeMaterializedRecord result;
			result.edge_id = record->edge_id;
			result.entity = {record->source_node_id, record->target_node_id, typeId};
			result.properties.owner_entity_id = record->edge_id;
			result.properties.owner_entity_type = Edge::typeId;
			result.properties.entries.reserve(record->properties.size());
			for (const LogicalGraphWriteProperty &property: record->properties) {
				const std::optional<uint32_t> token = resolvers.property_key_token(property.key);
				if (!token.has_value() || *token == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge materializer found an unresolved Property Token");
				}
				result.properties.entries.push_back({*token, property.value});
			}
			std::ranges::sort(result.properties.entries, {}, &PropertyCollectionRootEntry::key_token);
			if (std::ranges::adjacent_find(result.properties.entries, {}, &PropertyCollectionRootEntry::key_token) !=
				result.properties.entries.end()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge materializer found distinct Property names that resolve to one Token");
			}
			visitor(std::move(result));
		}
		if (materialized == 0U) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge materializer requires a nonempty sealed source");
		}
	}

} // namespace graph::storage
