/**
 * @file GlobalAdjacencyIndex.cpp
 * @brief Global ordinary-graph adjacency range reader.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"

#include <algorithm>
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
																		std::optional<GlobalAdjacencyContinuation> continuation,
			const std::function<void(size_t)> &retainedIdAdmission) const {
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
			const uint8_t invalidCursor = static_cast<uint8_t>(cursor.database_id != snapshot.superblock().database_id) |
                                                  static_cast<uint8_t>(cursor.generation != snapshot.superblock().generation) |
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

		size_t boundaryKeyBytes = 0U;
		const auto admitCapacity = [&](size_t capacity) {
			if (capacity > std::numeric_limits<size_t>::max() / sizeof(uint64_t)) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "global adjacency ID capacity overflowed");
			}
			const size_t idBytes = capacity * sizeof(uint64_t);
			if (boundaryKeyBytes > std::numeric_limits<size_t>::max() - idBytes)
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "global adjacency boundary capacity overflowed");
			if (retainedIdAdmission) retainedIdAdmission(idBytes + boundaryKeyBytes);
		};
		const auto admitBoundary = [&](size_t bytes) {
			boundaryKeyBytes = bytes;
			admitCapacity(result.edge_ids.capacity());
		};
		{
			BTreeIndex::RangeCursor cursor = index_.openRangeCursor(
				snapshot, lowerBound, maximumEntries + 1U, BufferPoolReadHint::BPRH_NORMAL);
		while (const auto entry = cursor.next(admitBoundary)) {
			if (entry->value_or_child != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "global adjacency tree decoded a nonempty leaf value");
			}
			const GlobalAdjacencyKey key = AdjacencyKeyCodec::decode(entry->key);
			if (key.direction != direction || key.node_id != nodeId) {
				break;
			}
			if (result.edge_ids.size() == maximumEntries) {
				result.continuation = {snapshot.superblock().generation, direction, nodeId, result.edge_ids.back(),
                    snapshot.superblock().database_id};
				break;
			}
			if (result.edge_ids.size() == result.edge_ids.capacity()) {
				const size_t capacity = result.edge_ids.capacity();
				const size_t next = capacity == 0U ? std::min<size_t>(maximumEntries, 16U)
					: (capacity > maximumEntries / 2U ? maximumEntries : capacity * 2U);
				if (next > std::numeric_limits<size_t>::max() - capacity) {
					throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "global adjacency growth peak overflowed");
				}
				admitCapacity(capacity + next);
				result.edge_ids.reserve(next);
				admitCapacity(result.edge_ids.capacity());
			}
			result.edge_ids.push_back(key.edge_id);
		}
		}
		// The cursor may still own a boundary key when lookahead stops this scan.
		// Reconcile only after its storage has actually been released.
		boundaryKeyBytes = 0U;
		admitCapacity(result.edge_ids.capacity());
		return result;
	}

	bool GlobalAdjacencyIndex::visitForNodes(const MetadataSnapshotPin &snapshot,
			const GlobalAdjacencyDirection direction, const std::span<const uint64_t> nodeIds,
			const size_t maximumEntries, const std::function<bool(uint64_t, uint64_t)> &visitor,
			const std::function<void(size_t)> &boundaryKeyAdmission) const {
		if (!snapshot.isValid() || !isDirection(direction) || nodeIds.empty() || maximumEntries == 0U || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "global adjacency multi-node scan requires a snapshot, nodes, bound, and visitor");
		}
		if (!std::ranges::is_sorted(nodeIds) || std::ranges::adjacent_find(nodeIds) != nodeIds.end() || nodeIds.front() == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "global adjacency multi-node scan requires strictly increasing Node IDs");
		}
		const auto lowerBound = AdjacencyKeyCodec::lowerBoundFixed(direction, nodeIds.front());
		size_t boundaryBytes = 0U;
		class AdmissionCheckpoint final : public CancellationCheckpoint {
		public:
			AdmissionCheckpoint(const std::function<void(size_t)> &admission, const size_t &bytes)
				: admission_(admission), bytes_(bytes) {}
			void check() const override { if (admission_) admission_(bytes_); }
		private:
			const std::function<void(size_t)> &admission_;
			const size_t &bytes_;
		} checkpoint(boundaryKeyAdmission, boundaryBytes);
		BTreeIndex::RangeCursor cursor = index_.openRangeCursor(snapshot, lowerBound,
			maximumEntries, BufferPoolReadHint::BPRH_SEQUENTIAL, false,
			boundaryKeyAdmission ? &checkpoint : nullptr);
		size_t scanned = 0U;
		size_t matched = 0U;
		for (;;) {
			// The B+Tree cursor bounds physical work, including keys belonging to
			// unrequested nodes between the first and last source. Stop before asking
			// the cursor for an entry beyond that budget so resource exhaustion is a
			// normal incomplete result rather than an internal cursor exception.
			if (scanned == maximumEntries) {
				return false;
			}
			const std::optional<BTreePageEntry> entry = cursor.next([&](size_t bytes) {
				boundaryBytes = bytes;
				if (boundaryKeyAdmission) boundaryKeyAdmission(bytes);
			});
			if (!entry.has_value()) {
				return true;
			}
			if (entry->key.capacity() > std::numeric_limits<size_t>::max() - boundaryBytes)
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "adjacency key admission overflowed");
			if (boundaryKeyAdmission) boundaryKeyAdmission(boundaryBytes + entry->key.capacity());
			++scanned;
			if (entry->value_or_child != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "global adjacency multi-node scan decoded a nonempty leaf value");
			}
			const GlobalAdjacencyKey key = AdjacencyKeyCodec::decode(entry->key);
			if (key.direction != direction) {
				return true;
			}
			if (key.node_id > nodeIds.back()) {
				return true;
			}
			if (!std::ranges::binary_search(nodeIds, key.node_id)) {
				continue;
			}
			if (matched == maximumEntries) {
				return false;
			}
			++matched;
			if (!visitor(key.node_id, key.edge_id)) {
				return false;
			}
		}
	}

} // namespace graph::storage
