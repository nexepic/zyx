/**
 * @file GlobalAdjacencyIndex.cpp
 * @brief Global ordinary-graph adjacency range reader.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"

#include <limits>

namespace graph::storage {

	namespace {

		[[nodiscard]] bool isDirection(GlobalAdjacencyDirection direction) {
			return direction == GlobalAdjacencyDirection::GAD_OUT || direction == GlobalAdjacencyDirection::GAD_IN;
		}

		void validateRangeIdentity(GlobalAdjacencyDirection direction, uint64_t nodeId) {
			if (!isDirection(direction) || nodeId == 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "global adjacency range requires a known direction and a nonzero Node ID");
			}
		}

	} // namespace

	GlobalAdjacencyIndex::GlobalAdjacencyIndex(BTreePageStore &pages, RelocatableMetadataStore &metadata) :
		index_(RSS_GLOBAL_ADJACENCY_INDEX_ID, pages, metadata, BTreeLeafValueLayout::BLVL_EMPTY) {}

	GlobalAdjacencyRange GlobalAdjacencyIndex::scan(const MetadataSnapshotPin &snapshot,
																		GlobalAdjacencyDirection direction, uint64_t nodeId,
																		size_t maximumEntries,
																		std::optional<GlobalAdjacencyContinuation> continuation) const {
		if (!snapshot.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "global adjacency range requires a live metadata snapshot");
		}
		validateRangeIdentity(direction, nodeId);
		GlobalAdjacencyRange result;
		if (maximumEntries == 0U) {
			return result;
		}
		if (maximumEntries == std::numeric_limits<size_t>::max()) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "global adjacency range limit cannot reserve a lookahead entry");
		}

		std::vector<std::byte> lowerBound;
		if (continuation.has_value()) {
			const GlobalAdjacencyContinuation &cursor = *continuation;
			const uint8_t invalidCursor = static_cast<uint8_t>(cursor.generation != snapshot.superblock().generation) |
											  static_cast<uint8_t>(cursor.direction != direction) |
											  static_cast<uint8_t>(cursor.node_id != nodeId) |
											  static_cast<uint8_t>(cursor.last_edge_id == 0U);
			if (invalidCursor != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "global adjacency continuation does not belong to this snapshot or range");
			}
			if (cursor.last_edge_id == std::numeric_limits<uint64_t>::max()) {
				return result;
			}
			lowerBound = AdjacencyKeyCodec::encode({direction, nodeId, cursor.last_edge_id + 1U});
		} else {
			lowerBound = AdjacencyKeyCodec::lowerBound(direction, nodeId);
		}

		const std::vector<BTreePageEntry> entries =
				index_.scan(snapshot, lowerBound, maximumEntries + 1U, BufferPoolReadHint::BPRH_NORMAL);
		result.edge_ids.reserve(std::min(maximumEntries, entries.size()));
		for (const BTreePageEntry &entry: entries) {
			if (entry.value_or_child != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "global adjacency tree decoded a nonempty leaf value");
			}
			const GlobalAdjacencyKey key = AdjacencyKeyCodec::decode(entry.key);
			if (key.direction != direction || key.node_id != nodeId) {
				break;
			}
			if (result.edge_ids.size() == maximumEntries) {
				result.continuation = {snapshot.superblock().generation, direction, nodeId, result.edge_ids.back()};
				break;
			}
			result.edge_ids.push_back(key.edge_id);
		}
		return result;
	}

} // namespace graph::storage
