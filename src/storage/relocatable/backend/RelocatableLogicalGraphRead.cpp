/**
 * @file RelocatableLogicalGraphRead.cpp
 * @brief relocatable implementation of the format-independent logical graph reader.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/utils/OwnedPmrObject.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/relocatable/adjacency/CoveredAdjacencyCandidate.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/debug/PerfTrace.hpp"

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

	class RelocatableLogicalGraphReadSnapshot final : public LogicalGraphVectorSearchSnapshot, public LogicalQueryScratchProvider, public utils::OwnedPmrObject {
	public:
		RelocatableLogicalGraphReadSnapshot(const RelocatableStorageBackend &backend,
																											const PageBackedLogicalGraphReadAdapter &nativeReads,
			std::shared_ptr<std::pmr::memory_resource> memory) :
			backend_(backend), native_reads_(nativeReads), snapshot_(backend.openSnapshot()), read_session_(std::move(memory), utils::OwnedAllocationScope::owner()) {}

        void enableCoveredControl(uint64_t indexId, size_t maximumEdges, size_t maximumMetadataPages,
            std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation) {
            if (covered_ || !memory || !backend_.isReadOnly())
                throw std::invalid_argument("covered candidate control requires a read-only backend and private account");
            utils::OwnedAllocationScope scope(memory);
            utils::OwnedHeapReservation frontier(memory, 65536U), boundary(memory);
            const auto report = CoveredAdjacencyCandidate::verifyIntegrity(*backend_.btree_pages_, *backend_.native_entities_,
                *backend_.metadata_, snapshot_.metadataSnapshot(), indexId, maximumEdges, maximumMetadataPages,
                [&](size_t bytes) { boundary.set(bytes); }, cancellation);
            const bool empty = !backend_.metadata_->findIndexRootOptional(snapshot_.metadataSnapshot(), indexId);
            if (empty && report.edges)
                throw FormatException(FormatError::FERR_INVALID_STATE, "covered candidate control has no complete source tree");
            covered_.emplace(CoveredControl{indexId, empty, std::move(memory)});
            debug::PerfTrace::addValue("logical_graph.covered_candidate.setup_edges", int64_t(report.edges));
        }

		[[nodiscard]] bool queryScratchEnabled() const override {return backend_.nativeQueryScratchEnabled();}
		[[nodiscard]] std::unique_ptr<LogicalQueryScratchSession> openQueryScratchSession(size_t bytes,size_t fragments) const override {
			return const_cast<RelocatableStorageBackend &>(backend_).openNativeQueryScratchSession(bytes,fragments);
		}
        [[nodiscard]] std::unique_ptr<LogicalQueryScratchSession> openQueryScratchSession(size_t bytes, size_t fragments,
                std::shared_ptr<query::execution::detail::ResourceAccount> account) const override {
            return const_cast<RelocatableStorageBackend &>(backend_).openNativeQueryScratchSession(bytes, fragments, std::move(account));
        }

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
				return readNodeShaped(nodeId, {.include_labels = true, .all_properties = true});
			}
			[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const override {
				return native_reads_.readNodeShaped(snapshot_.metadataSnapshot(), nodeId, shape, &read_session_);
			}
			[[nodiscard]] std::vector<LogicalGraphNode>
			readNodesShapedSorted(const std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const override {
				return native_reads_.readNodesShapedSorted(snapshot_.metadataSnapshot(), nodeIds, shape, &read_session_);
			}
			[[nodiscard]] bool supportsShapedPropertyReadAdmission() const override { return true; }
			[[nodiscard]] std::vector<LogicalGraphNode>
			readNodesShapedSortedWithAdmission(std::span<const uint64_t> nodeIds, const EntityReadShape &shape,
					const std::function<void(size_t)> &admission) const override {
				if (!admission) throw std::invalid_argument("shaped Property admission callback must not be empty");
				return native_reads_.readNodesShapedSorted(snapshot_.metadataSnapshot(), nodeIds, shape, &read_session_, &admission);
			}
			[[nodiscard]] std::vector<LogicalGraphEdge>
			readEdgesShapedSortedWithAdmission(std::span<const uint64_t> edgeIds, const EntityReadShape &shape,
					const std::function<void(size_t)> &admission) const override {
				if (!admission) throw std::invalid_argument("shaped Property admission callback must not be empty");
				return native_reads_.readEdgesShapedSorted(snapshot_.metadataSnapshot(), edgeIds, shape, &read_session_, &admission);
			}
			[[nodiscard]] PropertyValue readNodeProperty(uint64_t nodeId, const std::string_view key) const override {
				return native_reads_.readNodeProperty(snapshot_.metadataSnapshot(), nodeId, key, &read_session_);
			}
			[[nodiscard]] std::optional<bool> nodePropertyExists(uint64_t nodeId, const std::string_view key) const override {
				return native_reads_.nodePropertyExists(snapshot_.metadataSnapshot(), nodeId, key, &read_session_);
			}
			[[nodiscard]] bool supportsPropertyNonNullProbes() const override { return true; }
			[[nodiscard]] std::optional<bool> nodePropertyIsNonNull(uint64_t nodeId, const std::string_view key) const override {
				return native_reads_.nodePropertyExists(snapshot_.metadataSnapshot(), nodeId, key, &read_session_, true);
			}


			[[nodiscard]] std::vector<PropertyValue>
			readNodePropertiesSorted(const std::span<const uint64_t> nodeIds, const std::string_view key) const override {
				return native_reads_.readNodePropertiesSorted(snapshot_.metadataSnapshot(), nodeIds, key, &read_session_);
			}
			[[nodiscard]] std::vector<std::vector<PropertyValue>>
			readNodePropertyColumnsSorted(const std::span<const uint64_t> nodeIds,
																  const std::span<const std::string_view> keys) const override {
				return native_reads_.readNodePropertyColumnsSorted(snapshot_.metadataSnapshot(), nodeIds, keys, &read_session_);
			}
			[[nodiscard]] bool supportsPropertyReadAdmission() const override { return true; }
			[[nodiscard]] bool supportsNodePropertyCellBatches() const override { return true; }
			void readNodePropertyCellsSortedWithAdmission(std::span<const uint64_t> ids,
				std::span<const std::string_view> keys, const PropertyCellSink &sink,
				const std::function<void(size_t)> &admission) const override {
				if (!admission) throw std::invalid_argument("direct Property cells require admission");
				native_reads_.readNodePropertyCellsSorted(snapshot_.metadataSnapshot(), ids, keys,
					sink, &read_session_, &admission);
			}
            void readNodePropertyCellsWithScalarsSortedWithAdmission(std::span<const uint64_t> ids,
                std::span<const std::string_view> keys, const PropertyCellSink &sink,
                const PropertyScalarReadSink &scalars, const std::function<void(size_t)> &admission) const override {
                if (!sink || !scalars || !admission) throw std::invalid_argument("direct scalar cells require sinks and admission");
                native_reads_.readNodePropertyCellsSorted(snapshot_.metadataSnapshot(), ids, keys,
                    sink, &read_session_, &admission, &scalars);
            }
            void readNodePropertyCellsWithViewsSortedWithAdmission(std::span<const uint64_t> ids,
                std::span<const std::string_view> keys, const PropertyCellSink &sink,
                const PropertyScalarReadSink &scalars, const PropertyTextReadSink &texts,
                const std::function<void(size_t)> &admission) const override {
                if (!sink || !scalars || !texts || !admission)
                    throw std::invalid_argument("typed Property cells require sinks and admission");
                native_reads_.readNodePropertyCellsSorted(snapshot_.metadataSnapshot(), ids, keys,
                    sink, &read_session_, &admission, &scalars, &texts);
            }
            bool tryReadNodePropertyColumnBatchesSortedWithAdmission(std::span<const uint64_t> ids,
                std::span<const std::string_view> keys, const PropertyCellSink &values,
                const PropertyColumnBatchReadSinks &batches, const std::function<void(size_t)> &admission) const override {
                if (!values || !batches.scalars || !batches.texts || !admission)
                    throw std::invalid_argument("Property runs require sinks and admission");
                // These singleton fallbacks remain synchronous for complex,
                // native-field and unborrowable-page producer paths.
                const PropertyScalarReadSink scalar = [&](size_t row, size_t column, PropertyType type, uint64_t bits) {
                    batches.scalars(std::span(&row, 1U), column, type, std::span(&bits, 1U));
                };
                const PropertyTextReadSink text = [&](size_t row, size_t column, std::string_view value) {
                    batches.texts(std::span(&row, 1U), column, std::span(&value, 1U));
                };
                native_reads_.readNodePropertyCellsSorted(snapshot_.metadataSnapshot(), ids, keys,
                    values, &read_session_, &admission, &scalar, &text, &batches);
                return true;
            }
			[[nodiscard]] std::vector<std::vector<PropertyValue>>
			readNodePropertyColumnsSortedWithAdmission(std::span<const uint64_t> nodeIds,
					std::span<const std::string_view> keys, const std::function<void(size_t)> &admission) const override {
				if (!admission) throw std::invalid_argument("Property admission callback must not be empty");
				return native_reads_.readNodePropertyColumnsSorted(snapshot_.metadataSnapshot(), nodeIds, keys,
						&read_session_, &admission);
			}
			[[nodiscard]] std::vector<std::vector<PropertyValue>>
			readEdgePropertyColumnsSortedWithAdmission(std::span<const uint64_t> edgeIds,
					std::span<const std::string_view> keys, const std::function<void(size_t)> &admission) const override {
				if (!admission) throw std::invalid_argument("Property admission callback must not be empty");
				return native_reads_.readEdgePropertyColumnsSorted(snapshot_.metadataSnapshot(), edgeIds, keys,
						&read_session_, &admission);
			}
			[[nodiscard]] bool nodeHasLabels(uint64_t nodeId,
													 const std::vector<std::string> &labels) const override {
				return native_reads_.nodeHasLabels(snapshot_.metadataSnapshot(), nodeId, labels, &read_session_);
			}
			[[nodiscard]] std::vector<bool> readNodeLabelMatchesSorted(
					std::span<const uint64_t> nodeIds, const std::vector<std::string> &labels) const override {
				return native_reads_.readNodeLabelMatchesSorted(snapshot_.metadataSnapshot(), nodeIds, labels, &read_session_);
			}

			[[nodiscard]] EntityRef readNodeRef(uint64_t nodeId, const EntityReadShape &shape) const override {
				return native_reads_.readNodeRef(snapshot_.metadataSnapshot(), nodeId, shape, &read_session_);
			}
			void visitNodeRefsWithLabelMatches(size_t maximumRecords, const std::vector<std::string> &requiredLabels,
														  const std::function<bool(const EntityRef &, bool)> &visitor) const override {
				static_cast<void>(native_reads_.visitNodeRefsWithLabelMatchesUntil(
						snapshot_.metadataSnapshot(), maximumRecords, requiredLabels, visitor, &read_session_));
			}

			void visitNodeRefsForLabelPredicate(size_t maximumRecords,
					const std::vector<std::string> &labels,
					const std::function<bool(const EntityRef &, bool)> &visitor) const override {
				auto canonical = labels;
				std::ranges::sort(canonical);
				canonical.erase(std::ranges::unique(canonical).begin(), canonical.end());
				if (backend_.visitNativeNodeLabelCandidates(snapshot_.metadataSnapshot(), canonical,
						maximumRecords, visitor, &read_session_).has_value()) return;
				visitNodeRefsWithLabelMatches(maximumRecords, canonical, visitor);
			}

			[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
				return backend_.containsLogicalNode(snapshot_, nodeId, &read_session_);
			}
			[[nodiscard]] std::vector<bool> containsNodesSorted(std::span<const uint64_t> nodeIds) const override {
				return backend_.containsLogicalNodesSorted(snapshot_, nodeIds, &read_session_);
			}

			[[nodiscard]] bool supportsNodeExistenceProbes() const override { return true; }

			[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
				return readEdgeShaped(edgeId, {.include_type = true, .all_properties = true});
			}
			[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const override {
				return native_reads_.readEdgeShaped(snapshot_.metadataSnapshot(), edgeId, shape, &read_session_);
			}
			[[nodiscard]] std::vector<LogicalGraphEdge>
			readEdgesShapedSorted(const std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const override {
				return native_reads_.readEdgesShapedSorted(snapshot_.metadataSnapshot(), edgeIds, shape, &read_session_);
			}
			[[nodiscard]] PropertyValue readEdgeProperty(uint64_t edgeId, const std::string_view key) const override {
				return native_reads_.readEdgeProperty(snapshot_.metadataSnapshot(), edgeId, key, &read_session_);
			}
			[[nodiscard]] std::optional<bool> edgePropertyExists(uint64_t edgeId, const std::string_view key) const override {
				return native_reads_.edgePropertyExists(snapshot_.metadataSnapshot(), edgeId, key, &read_session_);
			}
			[[nodiscard]] std::optional<bool> edgePropertyIsNonNull(uint64_t edgeId, const std::string_view key) const override {
				return native_reads_.edgePropertyExists(snapshot_.metadataSnapshot(), edgeId, key, &read_session_, true);
			}


			[[nodiscard]] std::vector<PropertyValue>
			readEdgePropertiesSorted(const std::span<const uint64_t> edgeIds, const std::string_view key) const override {
				return native_reads_.readEdgePropertiesSorted(snapshot_.metadataSnapshot(), edgeIds, key, &read_session_);
			}
			[[nodiscard]] std::vector<std::vector<PropertyValue>>
			readEdgePropertyColumnsSorted(const std::span<const uint64_t> edgeIds,
																  const std::span<const std::string_view> keys) const override {
				return native_reads_.readEdgePropertyColumnsSorted(snapshot_.metadataSnapshot(), edgeIds, keys, &read_session_);
			}

			[[nodiscard]] EntityRef readEdgeRef(uint64_t edgeId, const EntityReadShape &shape) const override {
				return native_reads_.readEdgeRef(snapshot_.metadataSnapshot(), edgeId, shape, &read_session_);
			}

			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
			findNodesByExactProperties(const std::vector<std::string> &labels,
											 const std::vector<std::pair<std::string, PropertyValue>> &properties,
											 size_t maximumNodes, size_t maximumPropertyEntries) const override {
				return backend_.findNativeNodesByExactProperties(snapshot_.metadataSnapshot(), labels, properties,
																					 maximumNodes, maximumPropertyEntries, &read_session_);
			}

			[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByExactProperties(
					const std::vector<std::string> &labels,
					const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
					size_t maximumPropertyEntries) const override {
				return backend_.findNativeNodeCandidatesByExactProperties(snapshot_.metadataSnapshot(), labels, properties,
																					   maximumNodes, maximumPropertyEntries, &read_session_);
			}

			[[nodiscard]] bool supportsNativeExactNodeCandidates() const override { return true; }
			[[nodiscard]] std::optional<bool> visitNodeCandidatesByExactProperties(
					const std::vector<std::string> &labels,
					const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
					size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor) const override {
				return backend_.visitNativeNodeCandidatesByExactProperties(snapshot_.metadataSnapshot(), labels, properties,
						maximumNodes, maximumPropertyEntries, visitor, &read_session_);
			}
			[[nodiscard]] std::optional<bool> visitNodeCandidatesByExactProperties(
					const std::vector<std::string> &labels,
					const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
					size_t maximumPropertyEntries, size_t maximumCandidateBytes,
					const IndexCandidateBatchVisitor &visitor) const override {
				return backend_.visitNativeNodeCandidatesByExactProperties(snapshot_.metadataSnapshot(), labels, properties,
						maximumNodes, maximumPropertyEntries, visitor, &read_session_, maximumCandidateBytes);
			}

			[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByPropertyRange(
					const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
					size_t maximumNodes, size_t maximumPropertyEntries) const override {
				return backend_.findNativeNodeCandidatesByPropertyRange(snapshot_.metadataSnapshot(), labels, range,
						maximumNodes, maximumPropertyEntries, &read_session_);
			}

			[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByCompositePropertyRange(
					const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
					size_t maximumNodes, size_t maximumPropertyEntries) const override {
				return backend_.findNativeNodeCandidatesByCompositePropertyRange(snapshot_.metadataSnapshot(), labels, request,
						maximumNodes, maximumPropertyEntries, &read_session_);
			}

			[[nodiscard]] bool supportsNativeRangeNodeCandidates() const override { return true; }
			[[nodiscard]] std::optional<bool> visitNodeCandidatesByPropertyRange(
					const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range, size_t maximumNodes,
					size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor) const override {
				return backend_.visitNativeNodeCandidatesByPropertyRange(snapshot_.metadataSnapshot(), labels, range,
						maximumNodes, maximumPropertyEntries, visitor, &read_session_);
			}
			[[nodiscard]] std::optional<bool> visitNodeCandidatesByPropertyRange(
					const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range, size_t maximumNodes,
					size_t maximumPropertyEntries, size_t maximumCandidateBytes,
					const IndexCandidateBatchVisitor &visitor) const override {
				return backend_.visitNativeNodeCandidatesByPropertyRange(snapshot_.metadataSnapshot(), labels, range,
						maximumNodes, maximumPropertyEntries, visitor, &read_session_, maximumCandidateBytes);
			}
			[[nodiscard]] std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
					const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
					size_t maximumNodes, size_t maximumPropertyEntries,
					const IndexCandidateBatchVisitor &visitor) const override {
				return backend_.visitNativeNodeCandidatesByCompositePropertyRange(snapshot_.metadataSnapshot(), labels,
						request, maximumNodes, maximumPropertyEntries, visitor, &read_session_);
			}
			[[nodiscard]] std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
					const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
					size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
					const IndexCandidateBatchVisitor &visitor) const override {
				return backend_.visitNativeNodeCandidatesByCompositePropertyRange(snapshot_.metadataSnapshot(), labels,
						request, maximumNodes, maximumPropertyEntries, visitor, &read_session_, maximumCandidateBytes);
			}

			[[nodiscard]] std::optional<uint64_t> nativeSnapshotGeneration() const override {
				return snapshot_.metadataSnapshot().superblock().generation.value();
			}

			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
			findNodesByPropertyRange(const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
								 size_t maximumNodes, size_t maximumPropertyEntries) const override {
				return backend_.findNativeNodesByPropertyRange(snapshot_.metadataSnapshot(), labels, range, maximumNodes,
																				maximumPropertyEntries, &read_session_);
			}

			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByCompositePropertyRange(
					const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
					size_t maximumNodes, size_t maximumPropertyEntries) const override {
				return backend_.findNativeNodesByCompositePropertyRange(snapshot_.metadataSnapshot(), labels, request,
																				maximumNodes, maximumPropertyEntries, &read_session_);
			}

			void visitNodes(size_t maximumRecords,
							const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
				requireVisitor(visitor);
				static_cast<void>(native_reads_.visitNodesUntil(snapshot_.metadataSnapshot(), maximumRecords, visitor,
																												 &read_session_));
			}

			void visitNodeRefs(size_t maximumRecords,
									const std::function<bool(const EntityRef &)> &visitor) const override {
				if (!visitor) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "logical graph Node reference scan requires a visitor");
				}
				static_cast<void>(native_reads_.visitNodeRefsUntil(snapshot_.metadataSnapshot(), maximumRecords, visitor,
																						 &read_session_));
			}

			[[nodiscard]] bool supportsNativeNodeRefScans() const override { return true; }

			[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																																					  size_t maximumMetadataPages) const override {
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "logical graph resumable Node scan requires positive bounds");
				}
				return native_reads_.scanNodesAfter(snapshot_.metadataSnapshot(), afterNodeId, maximumRecords,
																																									 maximumMetadataPages, &read_session_);
			}

			[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																																					  size_t maximumMetadataPages,
																																					  const EntityReadShape &shape) const override {
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
												  "logical shaped Node scan requires positive bounds");
				}
				validateEntityReadShape(shape);
				return native_reads_.scanNodesAfter(snapshot_.metadataSnapshot(), afterNodeId, maximumRecords,
																																					 maximumMetadataPages, shape, &read_session_);
			}

		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
							  std::optional<LogicalGraphAdjacencyContinuation> continuation,
							  size_t maximumRecords) const override {
				return native_reads_.scanAdjacentEdges(snapshot_.metadataSnapshot(), nodeId, direction,
																									 std::move(continuation), maximumRecords, &read_session_);
			}

			[[nodiscard]] bool supportsAdjacencyPropertyReadAdmission() const override { return true; }
			[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithPropertyAdmission(
				uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
				const std::function<void(size_t)> &admission) const override {
				if (!admission) throw std::invalid_argument("adjacency Property admission callback must not be empty");
				return native_reads_.scanAdjacentEdges(snapshot_.metadataSnapshot(), nodeId, direction,
					std::move(continuation), maximumRecords, &read_session_, &admission);
			}

			[[nodiscard]] bool supportsAdjacencyReadAdmission() const override { return true; }
			[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithAdmission(
				uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
				const std::function<void(size_t)> &propertyAdmission,
				const std::function<void(size_t)> &bufferAdmission) const override {
				if (!propertyAdmission || !bufferAdmission) throw std::invalid_argument("adjacency admission callbacks must not be empty");
				return native_reads_.scanAdjacentEdges(snapshot_.metadataSnapshot(), nodeId, direction,
					std::move(continuation), maximumRecords, &read_session_, &propertyAdmission, bufferAdmission);
			}

			[[nodiscard]] bool supportsAdjacencyTopologyBufferAdmission() const override { return true; }
			[[nodiscard]] LogicalGraphAdjacencyTopologyPage
			scanAdjacentEdgeTopologiesWithBufferAdmission(uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
				const std::function<void(size_t)> &admission) const override {
                if (covered_) {
                    if (!admission) throw std::invalid_argument("covered topology admission callback is empty");
                    return coveredTopologies(nodeId, direction, continuation, maximumRecords, admission);
                }
				return native_reads_.scanAdjacentEdgeTopologies(snapshot_.metadataSnapshot(), nodeId, direction,
					std::move(continuation), maximumRecords, &read_session_, admission);
			}

			[[nodiscard]] LogicalGraphAdjacencyTopologyPage
				scanAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction,
											std::optional<LogicalGraphAdjacencyContinuation> continuation,
											size_t maximumRecords) const override {
                if (covered_) return coveredTopologies(nodeId, direction, continuation, maximumRecords, {});
				return native_reads_.scanAdjacentEdgeTopologies(snapshot_.metadataSnapshot(), nodeId, direction,
																	 std::move(continuation), maximumRecords, &read_session_);
			}

			[[nodiscard]] LogicalGraphAdjacencyReferencePage
			scanAdjacentEdgeRefs(uint64_t nodeId, LogicalGraphReadDirection direction,
						 std::optional<LogicalGraphAdjacencyContinuation> continuation,
						 size_t maximumRecords) const override {
                if (covered_) return coveredReferences(nodeId, direction, continuation, maximumRecords, {});
				return native_reads_.scanAdjacentEdgeRefs(snapshot_.metadataSnapshot(), nodeId, direction,
																	 std::move(continuation), maximumRecords, &read_session_);
			}

			[[nodiscard]] bool supportsAdjacencyReferenceBufferAdmission() const override { return true; }
			[[nodiscard]] LogicalGraphAdjacencyReferencePage
			scanAdjacentEdgeRefsWithBufferAdmission(uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
				const std::function<void(size_t)> &admission) const override {
                if (covered_) {
                    if (!admission) throw std::invalid_argument("covered reference admission callback is empty");
                    return coveredReferences(nodeId, direction, continuation, maximumRecords, admission);
                }
				return native_reads_.scanAdjacentEdgeRefs(snapshot_.metadataSnapshot(), nodeId, direction,
					std::move(continuation), maximumRecords, &read_session_, admission);
			}

			[[nodiscard]] bool supportsNativeAdjacentEdgeRefScans() const override { return true; }

			[[nodiscard]] bool supportsNativeAdjacentEdgeTopologyPages() const override { return true; }

			void visitEdges(size_t maximumRecords,
							const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				requireVisitor(visitor);
				static_cast<void>(native_reads_.visitEdgesUntil(snapshot_.metadataSnapshot(), maximumRecords, visitor,
																												 &read_session_));
			}

			void visitEdgeRefs(size_t maximumRecords,
									const std::function<bool(const EntityRef &)> &visitor) const override {
				if (!visitor) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "logical graph Edge reference scan requires a visitor");
				}
				static_cast<void>(native_reads_.visitEdgeRefsUntil(snapshot_.metadataSnapshot(), maximumRecords, visitor,
																						 &read_session_));
			}

			void visitEdgeRefsForTypePredicate(size_t maximumRecords, std::optional<uint64_t> typeToken,
					const std::function<bool(const EntityRef &)> &visitor) const override {
				if (typeToken.has_value() && backend_.visitNativeEdgeTypeCandidates(
						snapshot_.metadataSnapshot(), *typeToken, maximumRecords, visitor, &read_session_).has_value()) return;
				visitEdgeRefs(maximumRecords, visitor);
			}

			[[nodiscard]] bool supportsNativeEdgeRefScans() const override { return true; }

			[[nodiscard]] bool supportsNativeRelationshipTypeTokens() const override { return true; }

			[[nodiscard]] std::optional<uint64_t>
			findRelationshipTypeToken(const std::string_view name) const override {
				if (name.empty()) {
					return std::nullopt;
				}
				read_session_.bind(snapshot_.metadataSnapshot());
				if (const auto cached = read_session_.relationshipTypeToken(name); cached.has_value()) {
					return *cached;
				}
				const std::optional<uint64_t> token = backend_.findRelationshipTypeTokenId(snapshot_, name);
				read_session_.putRelationshipTypeToken(name, token);
				return token;
			}

			[[nodiscard]] bool supportsPropertyPresenceProbes() const override { return true; }
			[[nodiscard]] bool supportsPropertyTokenResolution() const override { return true; }

			[[nodiscard]] std::optional<uint32_t> findPropertyToken(const std::string_view name) const override {
				return backend_.findPropertyKeyToken(snapshot_, name);
			}

			void visitEdgeTopologies(
					size_t maximumRecords, const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
				if (!visitor) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "logical graph Edge topology scan requires a visitor");
				}
				static_cast<void>(
						native_reads_.visitEdgeTopologiesUntil(snapshot_.metadataSnapshot(), maximumRecords, visitor,
																																	 &read_session_));
			}

			void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
									const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				requireVisitor(visitor);
				static_cast<void>(native_reads_.visitAdjacentEdgesUntil(snapshot_.metadataSnapshot(), nodeId, direction,
																															maximumEdges, visitor, &read_session_));
			}

			void visitAdjacentEdgeTopologies(
					uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
					const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
				if (!visitor) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "logical graph adjacency topology scan requires a visitor");
				}
				static_cast<void>(native_reads_.visitAdjacentEdgeTopologiesUntil(snapshot_.metadataSnapshot(), nodeId,
																											 direction, maximumEdges, visitor, &read_session_));
			}

			void visitAdjacentEdgeRefsForNodes(std::span<const uint64_t> nodeIds, LogicalGraphReadDirection direction,
												 size_t maximumEdges,
												 const std::function<bool(const EntityRef &)> &visitor) const override {
				if (!visitor) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "logical graph batch adjacency reference scan requires a visitor");
				}
                if (covered_) { visitCoveredReferences(nodeIds, direction, maximumEdges, visitor, {}); return; }
				static_cast<void>(native_reads_.visitAdjacentEdgeRefsForNodesUntil(snapshot_.metadataSnapshot(), nodeIds,
																		 direction, maximumEdges, visitor, &read_session_));
			}

			[[nodiscard]] bool supportsBatchAdjacencyReferenceBufferAdmission() const override { return true; }
			void visitAdjacentEdgeRefsForNodesWithBufferAdmission(std::span<const uint64_t> nodeIds,
				LogicalGraphReadDirection direction, size_t maximumEdges,
				const std::function<bool(const EntityRef &)> &visitor,
				const std::function<void(size_t)> &admission) const override {
                if (covered_) {
                    if (!admission) throw std::invalid_argument("covered batch admission callback is empty");
                    visitCoveredReferences(nodeIds, direction, maximumEdges, visitor, admission); return;
                }
				static_cast<void>(native_reads_.visitAdjacentEdgeRefsForNodesUntil(snapshot_.metadataSnapshot(), nodeIds,
					direction, maximumEdges, visitor, &read_session_, admission));
			}

			[[nodiscard]] bool supportsNativeBatchAdjacentEdgeRefs() const override { return true; }

            [[nodiscard]] bool hasImmutableReadIdentity() const noexcept override { return true; }

			[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
				const Superblock &superblock = snapshot_.superblock();
				if (!superblock.database_id.isValid() || !superblock.generation.isValid()) { // ZYX_COV_EXCL_LINE:
					// SuperblockCodec rejects either invalid field before MetadataSnapshotPin is created.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable logical snapshot has no valid optimistic mutation fence");
				}
				return RelocatableStorageBackend::makeLogicalGraphMutationFence(superblock);
			}

			[[nodiscard]] std::optional<LogicalGraphSchemaIdentity> bindingSchemaIdentity() const override {
				static_cast<void>(mutationFence()); // authenticates the pinned metadata identity
				return RelocatableStorageBackend::makeLogicalGraphSchemaIdentity(snapshot_.superblock());
			}

			[[nodiscard]] LogicalGraphVectorSearchResult
			searchLogicalVector(const LogicalGraphVectorSearchRequest &request) const override {
				return backend_.searchLogicalVector(snapshot_, request);
			}

			[[nodiscard]] std::optional<LogicalGraphVectorIndexDescriptor>
			describeLogicalVectorIndex(const LogicalGraphVectorSearchRequest &request) const override {
				return backend_.describeLogicalVectorIndex(snapshot_, request);
			}

			// The native exact scorer needs only the descriptor's label and vector.
			[[nodiscard]] EntityReadShape exactOverlayCandidateReadShape(
					const LogicalGraphVectorIndexDescriptor &index) const override {
				const auto token = findPropertyToken(index.property_key);
				if (!token.has_value()) return LogicalGraphVectorSearchSnapshot::exactOverlayCandidateReadShape(index);
				return {.include_labels = true, .property_tokens = {*token}};
			}

			[[nodiscard]] std::optional<LogicalGraphVectorSearchHit>
			scoreLogicalVectorExactOverlayCandidate(const LogicalGraphVectorSearchRequest &request,
													   const LogicalGraphVectorIndexDescriptor &index,
													   const LogicalGraphNode &node) const override {
				return backend_.scoreLogicalVectorExactOverlayCandidate(snapshot_, request, index, node);
			}

	private:
        struct CoveredControl { uint64_t index; bool empty; std::shared_ptr<std::pmr::memory_resource> memory; };
        std::optional<CoveredControl> covered_;
        [[nodiscard]] LogicalGraphAdjacencyTopologyPage coveredTopologies(uint64_t node,
            LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
            size_t maximumRecords, const std::function<void(size_t)> &admission) const {
            size_t referenceBytes = 0, typeBytes = 0;
            auto references = coveredReferences(node, direction, continuation, maximumRecords,
                [&](size_t bytes) { referenceBytes = bytes; if (admission) admission(bytes); });
            const auto multiply = [](size_t count, size_t width) {
                if (count > (std::numeric_limits<size_t>::max)() / width)
                    throw std::overflow_error("covered topology buffer size overflow");
                return count * width;
            };
            const auto add = [](size_t a, size_t b) { return utils::detail::addStandardHeapBytes(a, b); };
            const auto admit = [&](size_t bytes) { if (admission) admission(bytes); };
            LogicalGraphAdjacencyTopologyPage result;
            admit(add(referenceBytes, multiply(references.edges.size(), sizeof(LogicalGraphEdgeTopology))));
            result.edges.reserve(references.edges.size());
            const auto vectors = add(referenceBytes, multiply(result.edges.capacity(), sizeof(LogicalGraphEdgeTopology)));
            admit(vectors);
            const auto &pin = snapshot_.metadataSnapshot(); read_session_.bind(pin);
            for (const auto &edge : references.edges) {
                std::string type;
                if (edge.type_token) {
                    const auto nameAdmission = [&](size_t bytes) { admit(add(add(vectors, typeBytes), bytes)); };
                    const auto kind = NativeReadSession::TokenNameKind::TNK_RELATIONSHIP_TYPE;
                    bool remember = false;
                    if (auto cached = read_session_.tokenName(kind, edge.type_token, nameAdmission)) type = std::move(*cached);
                    else {
                        type = backend_.resolveRelationshipTypeTokenWithNameAdmission(pin, edge.type_token, nameAdmission);
                        remember = true;
                    }
                    if (type.empty()) throw FormatException(FormatError::FERR_INVALID_STATE, "covered topology has an unresolved relationship type");
                    if (remember) read_session_.putTokenName(kind, edge.type_token, type);
                }
                typeBytes = add(typeBytes, type.capacity()); admit(add(vectors, typeBytes));
                result.edges.push_back({edge.entity_id, edge.source_node_id, edge.target_node_id, std::move(type)});
            }
            result.continuation = references.continuation;
            std::vector<EntityRef>().swap(references.edges);
            admit(add(multiply(result.edges.capacity(), sizeof(LogicalGraphEdgeTopology)), typeBytes));
            debug::PerfTrace::addValue("logical_graph.covered_candidate.topology_pages", 1);
            return result;
        }
        void visitCoveredReferences(std::span<const uint64_t> nodes, LogicalGraphReadDirection direction,
            size_t maximum, const std::function<bool(const EntityRef &)> &visitor,
            const std::function<void(size_t)> &admission) const {
            if (nodes.empty() || !nodes.front() || !maximum || maximum == (std::numeric_limits<size_t>::max)() || !visitor ||
                !std::ranges::is_sorted(nodes) || std::ranges::adjacent_find(nodes) != nodes.end())
                throw FormatException(FormatError::FERR_INVALID_HEADER, "covered batch requires sorted unique Nodes, a finite bound and a visitor");
            size_t visited = 0;
            for (const auto node : nodes) {
                std::optional<LogicalGraphAdjacencyContinuation> continuation;
                do {
                    auto page = coveredReferences(node, direction, continuation,
                        (std::max)(size_t{1}, (std::min)(maximum - visited, size_t{64})), admission);
                    for (const auto &edge : page.edges) {
                        if (visited == maximum)
                            throw FormatException(FormatError::FERR_INVALID_STATE, "covered batch exceeds its total Edge bound");
                        ++visited;
                        if (!visitor(edge)) {
                            page = {}; if (admission) admission(0); return;
                        }
                    }
                    continuation = page.continuation; page = {};
                    if (admission) admission(0);
                } while (continuation);
            }
        }
        [[nodiscard]] LogicalGraphAdjacencyReferencePage coveredReferences(uint64_t node,
            LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
            size_t maximumRecords, const std::function<void(size_t)> &admission) const {
            const auto &control = *covered_;
            utils::OwnedAllocationScope scope(control.memory);
            utils::OwnedHeapReservation frontier(control.memory, 32768U), boundary(control.memory);
            const auto &pin = snapshot_.metadataSnapshot();
            if (!node || !maximumRecords || (direction != LogicalGraphReadDirection::LGRD_OUTGOING &&
                direction != LogicalGraphReadDirection::LGRD_INCOMING))
                throw std::invalid_argument("covered candidate reference scan requires a Node, direction and bound");
            if (continuation && (!continuation->previous_edge_id || continuation->next_edge_id <= continuation->previous_edge_id))
                throw std::invalid_argument("covered candidate continuation has an invalid Edge order");
            const auto diskDirection = direction == LogicalGraphReadDirection::LGRD_OUTGOING ?
                GlobalAdjacencyDirection::GAD_OUT : GlobalAdjacencyDirection::GAD_IN;
            const auto admitBoundary = [&](size_t bytes) { boundary.set(bytes); };
            if (control.empty) {
                static_cast<void>(backend_.native_entities_->readNode(pin, node));
                if (continuation) throw std::invalid_argument("empty covered candidate cannot resume adjacency");
                if (admission) admission(0);
                debug::PerfTrace::addValue("logical_graph.covered_candidate.reference_pages", 1);
                return {};
            }
            const auto identity = [&](uint64_t after, uint64_t first) {
                return AdjacencyTopologyIdentity{pin.superblock().database_id.value(), pin.superblock().generation.value(),
                    node, after, direction, first};
            };
            if (continuation) {
                // The logical continuation has no storage provenance. Prove its
                // previous member and exact next member against this pin, too.
                const auto prior = CoveredAdjacencyCandidate::scan(*backend_.btree_pages_, *backend_.native_entities_,
                    *backend_.metadata_, pin, control.index, diskDirection, node, 1,
                    continuation->previous_edge_id == 1U ? std::nullopt :
                        std::optional(identity(continuation->previous_edge_id - 1U, continuation->previous_edge_id)), admitBoundary);
                if (prior.topology.entries().empty() || prior.topology.entries().front().entity_id != continuation->previous_edge_id ||
                    !prior.continuation || prior.continuation->expected_first_edge_id != continuation->next_edge_id)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "covered candidate did not resume an exact logical adjacency pair");
                debug::PerfTrace::addValue("logical_graph.covered_candidate.continuation_records", int64_t(prior.candidate_records));
            }
            const auto page = CoveredAdjacencyCandidate::scan(*backend_.btree_pages_, *backend_.native_entities_,
                *backend_.metadata_, pin, control.index, diskDirection, node, (std::min)(maximumRecords, size_t{64}),
                continuation ? std::optional(identity(continuation->previous_edge_id, continuation->next_edge_id)) : std::nullopt,
                admitBoundary);
            const auto entries = page.topology.entries();
            LogicalGraphAdjacencyReferencePage result;
            if (admission) admission(entries.size() * sizeof(EntityRef));
            result.edges.reserve(entries.size());
            if (admission) admission(result.edges.capacity() * sizeof(EntityRef));
            result.edges.insert(result.edges.end(), entries.begin(), entries.end());
            if (page.continuation) result.continuation = LogicalGraphAdjacencyContinuation{
                page.continuation->after_edge_id, page.continuation->expected_first_edge_id};
            debug::PerfTrace::addValue("logical_graph.covered_candidate.reference_pages", 1);
            debug::PerfTrace::addValue("logical_graph.covered_candidate.candidate_records", int64_t(page.candidate_records));
            return result;
        }

		const RelocatableStorageBackend &backend_;
		const PageBackedLogicalGraphReadAdapter &native_reads_;
			RelocatableReadSnapshot snapshot_;
		mutable NativeReadSession read_session_;
	};

	static_assert(alignof(RelocatableLogicalGraphReadSnapshot) <= alignof(std::max_align_t));

	std::unique_ptr<LogicalGraphReadSnapshot> RelocatableStorageBackend::openLogicalGraphReadSnapshot() const {
		requireOpen();
		if (native_logical_graph_reads_ == nullptr) {
			throw std::logic_error("relocatable backend has no native logical graph reader");
		}
		return std::unique_ptr<RelocatableLogicalGraphReadSnapshot>(
			new (utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : native_cache_memory_)
                RelocatableLogicalGraphReadSnapshot(*this, *native_logical_graph_reads_, native_cache_memory_));
	}

    std::unique_ptr<LogicalGraphReadSnapshot> RelocatableStorageBackend::openCoveredAdjacencyCandidateReadSnapshot(
        uint64_t indexId, size_t maximumEdges, size_t maximumEdgeMetadataPages,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation) const {
        requireOpen();
        if (!isReadOnly() || !memory || !native_logical_graph_reads_ || !btree_pages_ || !native_entities_)
            throw std::invalid_argument("covered candidate read control requires physical read-only native storage and an account");
        utils::OwnedAllocationScope scope(memory);
        auto snapshot = std::unique_ptr<RelocatableLogicalGraphReadSnapshot>(new (memory)
            RelocatableLogicalGraphReadSnapshot(*this, *native_logical_graph_reads_, native_cache_memory_));
        snapshot->enableCoveredControl(indexId, maximumEdges, maximumEdgeMetadataPages, std::move(memory), cancellation);
        return snapshot;
    }

	std::unique_ptr<LogicalGraphVectorSearchSnapshot>
	RelocatableStorageBackend::openLogicalGraphVectorSearchSnapshot() const {
		requireOpen();
		if (native_logical_graph_reads_ == nullptr) {
			throw std::logic_error("relocatable backend has no native logical graph reader");
		}
		return std::unique_ptr<RelocatableLogicalGraphReadSnapshot>(
			new (utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : native_cache_memory_)
                RelocatableLogicalGraphReadSnapshot(*this, *native_logical_graph_reads_, native_cache_memory_));
	}

} // namespace graph::storage
