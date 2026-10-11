/**
 * @file RelocatableLogicalGraphRead.cpp
 * @brief relocatable implementation of the format-independent logical graph reader.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::storage {

	namespace {

		void requireVisitor(const std::function<bool(const LogicalGraphNode &)> &visitor) {
			if (!visitor) {
				throw FormatException(FormatError::FERR_INVALID_HEADER, "logical graph Node scan requires a visitor");
			}
		}

		void requireVisitor(const std::function<bool(const LogicalGraphEdge &)> &visitor) {
			if (!visitor) {
				throw FormatException(FormatError::FERR_INVALID_HEADER, "logical graph Edge scan requires a visitor");
			}
		}
	} // namespace

	class RelocatableLogicalGraphReadSnapshot final : public LogicalGraphVectorSearchSnapshot {
	public:
		RelocatableLogicalGraphReadSnapshot(const RelocatableStorageBackend &backend,
																											const PageBackedLogicalGraphReadAdapter &nativeReads) :
			backend_(backend), native_reads_(nativeReads), snapshot_(backend.openSnapshot()) {}

			[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
				return native_reads_.readNode(snapshot_.metadataSnapshot(), nodeId);
			}

			[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
				return backend_.containsLogicalNode(snapshot_, nodeId);
			}

			[[nodiscard]] bool supportsNodeExistenceProbes() const override { return true; }

			[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
				return native_reads_.readEdge(snapshot_.metadataSnapshot(), edgeId);
			}

			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
			findNodesByExactProperties(const std::vector<std::string> &labels,
												 const std::vector<std::pair<std::string, PropertyValue>> &properties,
												 size_t maximumNodes, size_t maximumPropertyEntries) const override {
				return backend_.findNativeNodesByExactProperties(snapshot_.metadataSnapshot(), labels, properties,
																				 maximumNodes, maximumPropertyEntries);
			}

			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
			findNodesByPropertyRange(const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
								 size_t maximumNodes, size_t maximumPropertyEntries) const override {
				return backend_.findNativeNodesByPropertyRange(snapshot_.metadataSnapshot(), labels, range, maximumNodes,
																									maximumPropertyEntries);
			}

			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByCompositePropertyRange(
					const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
					size_t maximumNodes, size_t maximumPropertyEntries) const override {
				return backend_.findNativeNodesByCompositePropertyRange(snapshot_.metadataSnapshot(), labels, request,
																									maximumNodes, maximumPropertyEntries);
			}

			void visitNodes(size_t maximumRecords,
							const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
				requireVisitor(visitor);
				static_cast<void>(native_reads_.visitNodesUntil(snapshot_.metadataSnapshot(), maximumRecords, visitor));
			}

			[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
															  size_t maximumMetadataPages) const override {
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "logical graph resumable Node scan requires positive bounds");
				}
				return native_reads_.scanNodesAfter(snapshot_.metadataSnapshot(), afterNodeId, maximumRecords,
																									 maximumMetadataPages);
			}

		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
							  std::optional<LogicalGraphAdjacencyContinuation> continuation,
							  size_t maximumRecords) const override {
				if (continuation.has_value()) {
					std::lock_guard lock(adjacency_cursor_mutex_);
					const AdjacencyCursorKey key{nodeId, direction, continuation->previous_edge_id,
																															 continuation->next_edge_id};
					const auto iterator = adjacency_cursors_.find(key);
					if (iterator == adjacency_cursors_.end()) {
						// A continuation obtained outside this snapshot cache contains only
						// logical IDs. Revalidate it through the ordinary bounded scan
						// rather than accepting a stale physical cursor.
						return native_reads_.scanAdjacentEdges(snapshot_.metadataSnapshot(), nodeId, direction,
																																		   std::move(continuation), maximumRecords);
					}
					PageBackedLogicalGraphAdjacencyCursor cursor = std::move(iterator->second.cursor);
					adjacency_cursors_.erase(iterator);
					LogicalGraphAdjacencyPage page = cursor.next(snapshot_.metadataSnapshot(), std::move(continuation),
																																				 maximumRecords);
					cacheAdjacencyCursor(nodeId, direction, std::move(cursor), page.continuation);
					return page;
				}

				std::lock_guard lock(adjacency_cursor_mutex_);
				PageBackedLogicalGraphAdjacencyCursor cursor =
						native_reads_.openAdjacentEdgeCursor(snapshot_.metadataSnapshot(), nodeId, direction);
				LogicalGraphAdjacencyPage page = cursor.next(snapshot_.metadataSnapshot(), std::nullopt, maximumRecords);
				cacheAdjacencyCursor(nodeId, direction, std::move(cursor), page.continuation);
				return page;
			}

			void visitEdges(size_t maximumRecords,
							const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				requireVisitor(visitor);
				static_cast<void>(native_reads_.visitEdgesUntil(snapshot_.metadataSnapshot(), maximumRecords, visitor));
			}

			void visitEdgeTopologies(
					size_t maximumRecords, const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
				if (!visitor) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "logical graph Edge topology scan requires a visitor");
				}
				static_cast<void>(
						native_reads_.visitEdgeTopologiesUntil(snapshot_.metadataSnapshot(), maximumRecords, visitor));
			}

			void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
									const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				requireVisitor(visitor);
				static_cast<void>(native_reads_.visitAdjacentEdgesUntil(snapshot_.metadataSnapshot(), nodeId, direction,
																		maximumEdges, visitor));
			}

			void visitAdjacentEdgeTopologies(
					uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
					const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
				if (!visitor) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "logical graph adjacency topology scan requires a visitor");
				}
				static_cast<void>(native_reads_.visitAdjacentEdgeTopologiesUntil(snapshot_.metadataSnapshot(), nodeId,
																				 direction, maximumEdges, visitor));
			}

			[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
				const Superblock &superblock = snapshot_.superblock();
				if (!superblock.database_id.isValid() || !superblock.generation.isValid()) { // ZYX_COV_EXCL_LINE:
					// SuperblockCodec rejects either invalid field before MetadataSnapshotPin is created.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable logical snapshot has no valid optimistic mutation fence");
				}
				return RelocatableStorageBackend::makeLogicalGraphMutationFence(superblock);
			}

			[[nodiscard]] LogicalGraphVectorSearchResult
			searchLogicalVector(const LogicalGraphVectorSearchRequest &request) const override {
				return backend_.searchLogicalVector(snapshot_, request);
			}

			[[nodiscard]] std::optional<LogicalGraphVectorIndexDescriptor>
			describeLogicalVectorIndex(const LogicalGraphVectorSearchRequest &request) const override {
				return backend_.describeLogicalVectorIndex(snapshot_, request);
			}

			[[nodiscard]] std::optional<LogicalGraphVectorSearchHit>
			scoreLogicalVectorExactOverlayCandidate(const LogicalGraphVectorSearchRequest &request,
													   const LogicalGraphVectorIndexDescriptor &index,
													   const LogicalGraphNode &node) const override {
				return backend_.scoreLogicalVectorExactOverlayCandidate(snapshot_, request, index, node);
			}

	private:
		struct AdjacencyCursorKey {
			uint64_t node_id = 0U;
			LogicalGraphReadDirection direction = LogicalGraphReadDirection::LGRD_OUTGOING;
			uint64_t previous_edge_id = 0U;
			uint64_t next_edge_id = 0U;

			constexpr bool operator==(const AdjacencyCursorKey &) const = default;
		};

		struct AdjacencyCursorKeyHash {
			[[nodiscard]] size_t operator()(const AdjacencyCursorKey &key) const noexcept {
				const size_t first = std::hash<uint64_t>{}(key.node_id);
				const size_t second = std::hash<uint64_t>{}(key.previous_edge_id);
				const size_t third = std::hash<uint64_t>{}(key.next_edge_id);
				const size_t direction = std::hash<uint8_t>{}(static_cast<uint8_t>(key.direction));
				return first ^ (second << 1U) ^ (third << 7U) ^ (direction << 13U);
			}
		};

		struct AdjacencyCursorEntry {
			PageBackedLogicalGraphAdjacencyCursor cursor;
			uint64_t last_use = 0U;
		};

		void cacheAdjacencyCursor(uint64_t nodeId, LogicalGraphReadDirection direction,
									  PageBackedLogicalGraphAdjacencyCursor cursor,
									  const std::optional<LogicalGraphAdjacencyContinuation> &continuation) const {
			if (!continuation.has_value() || options().maximum_snapshot_adjacency_cursors == 0U) {
				return;
			}
			if (++adjacency_cursor_use_ == 0U) {
				// Monotonic ordering is only an eviction hint. Resetting the small
				// cache at wrap keeps eviction deterministic without relying on a
				// wrapped timestamp ordering.
				adjacency_cursors_.clear();
				++adjacency_cursor_use_;
			}
			if (adjacency_cursors_.size() == options().maximum_snapshot_adjacency_cursors) {
				const auto leastRecent = std::min_element(
						adjacency_cursors_.begin(), adjacency_cursors_.end(),
						[](const auto &left, const auto &right) { return left.second.last_use < right.second.last_use; });
				adjacency_cursors_.erase(leastRecent);
			}
			adjacency_cursors_.insert_or_assign(
					AdjacencyCursorKey{nodeId, direction, continuation->previous_edge_id,
																continuation->next_edge_id},
					AdjacencyCursorEntry{std::move(cursor), adjacency_cursor_use_});
		}

		[[nodiscard]] const RelocatableStorageBackendOptions &options() const { return backend_.options(); }

		const RelocatableStorageBackend &backend_;
		const PageBackedLogicalGraphReadAdapter &native_reads_;
		RelocatableReadSnapshot snapshot_;
		mutable std::mutex adjacency_cursor_mutex_;
		mutable std::unordered_map<AdjacencyCursorKey, AdjacencyCursorEntry, AdjacencyCursorKeyHash>
				adjacency_cursors_;
		mutable uint64_t adjacency_cursor_use_ = 0U;
	};

	std::unique_ptr<LogicalGraphReadSnapshot> RelocatableStorageBackend::openLogicalGraphReadSnapshot() const {
		requireOpen();
		if (native_logical_graph_reads_ == nullptr) {
			throw std::logic_error("relocatable backend has no native logical graph reader");
		}
		return std::make_unique<RelocatableLogicalGraphReadSnapshot>(*this, *native_logical_graph_reads_);
	}

	std::unique_ptr<LogicalGraphVectorSearchSnapshot>
	RelocatableStorageBackend::openLogicalGraphVectorSearchSnapshot() const {
		requireOpen();
		if (native_logical_graph_reads_ == nullptr) {
			throw std::logic_error("relocatable backend has no native logical graph reader");
		}
		return std::make_unique<RelocatableLogicalGraphReadSnapshot>(*this, *native_logical_graph_reads_);
	}

} // namespace graph::storage
