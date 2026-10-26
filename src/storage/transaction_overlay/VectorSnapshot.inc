	class LogicalGraphTransactionOverlay::VectorSnapshot final : public LogicalGraphVectorSearchSnapshot {
		[[nodiscard]] auto readRevisionCheckpoint() const {
			const uint64_t revision = overlay_.logical_revision_;
			return [this, revision] {
				requireActive();
				if (overlay_.logical_revision_ != revision)
					throw std::runtime_error("logical transaction overlay changed during read callback");
			};
		}

		[[nodiscard]] std::function<void(size_t)> guardReadAdmission(const std::function<void(size_t)> &admission) const {
			return [check = readRevisionCheckpoint(), &admission](size_t bytes) {
				check();
				admission(bytes);
				check();
			};
		}

		template<typename Visitor>
		[[nodiscard]] auto guardReadVisitor(const Visitor &visitor) const {
			if (!visitor) throw std::invalid_argument("overlay read visitor must not be empty");
			return [check = readRevisionCheckpoint(), &visitor](const auto &...args) {
				check();
				const bool keepGoing = visitor(args...);
				check();
				return keepGoing;
			};
		}

	public:
		explicit VectorSnapshot(const LogicalGraphTransactionOverlay &overlay,
									 LogicalGraphVectorSearchSnapshot &derivedSnapshot) :
				overlay_(overlay), derived_snapshot_(derivedSnapshot) {}


        // An unchanged overlay has exactly its pinned source's cells. Once a
        // mutation is staged, the existing owning overlay adapter must merge it.
        [[nodiscard]] bool supportsNodePropertyCellBatches() const override {
            requireActive();
            return overlay_.logical_revision_ == 0U && derived_snapshot_.supportsNodePropertyCellBatches();
        }
        void readNodePropertyCellsSortedWithAdmission(std::span<const uint64_t> ids,
            std::span<const std::string_view> keys, const PropertyCellSink &values,
            const std::function<void(size_t)> &admission) const override {
            if (!supportsNodePropertyCellBatches()) throw std::logic_error("changed overlay requires owning Property adapter");
            if (!values || !admission) throw std::invalid_argument("overlay direct cells require sink and admission");
            const auto check = readRevisionCheckpoint();
            const auto guarded = [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                check(); values(row, column, std::move(value)); check();
            };
            const auto admit = guardReadAdmission(admission);
            derived_snapshot_.readNodePropertyCellsSortedWithAdmission(ids, keys, guarded, admit);
            check();
        }
        void readNodePropertyCellsWithScalarsSortedWithAdmission(std::span<const uint64_t> ids,
            std::span<const std::string_view> keys, const PropertyCellSink &values,
            const PropertyScalarReadSink &scalars, const std::function<void(size_t)> &admission) const override {
            if (!supportsNodePropertyCellBatches()) throw std::logic_error("changed overlay requires owning Property adapter");
            if (!values || !scalars || !admission) throw std::invalid_argument("overlay direct scalars require sinks and admission");
            const auto check = readRevisionCheckpoint();
            const auto guarded = [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                check(); values(row, column, std::move(value)); check();
            };
            const auto scalar = [&](size_t row, size_t column, PropertyType type, uint64_t word) {
                check(); scalars(row, column, type, word); check();
            };
            const auto admit = guardReadAdmission(admission);
            derived_snapshot_.readNodePropertyCellsWithScalarsSortedWithAdmission(ids, keys, guarded, scalar, admit);
            check();
        }

		[[nodiscard]] bool supportsNativeNodeRefScans() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeNodeRefScans()
				: overlaySnapshot().supportsNativeNodeRefScans();
		}

		void visitNodeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitNodeRefs(maximumRecords, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitNodeRefs(maximumRecords, guarded);
			}
		}

		void visitNodeRefsWithLabelMatches(size_t maximumRecords, const std::vector<std::string> &labels,
			const std::function<bool(const EntityRef &, bool)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitNodeRefsWithLabelMatches(maximumRecords, labels, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitNodeRefsWithLabelMatches(maximumRecords, labels, guarded);
			}
		}

		[[nodiscard]] bool supportsNativeEdgeRefScans() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeEdgeRefScans()
				: overlaySnapshot().supportsNativeEdgeRefScans();
		}

		void visitEdgeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitEdgeRefs(maximumRecords, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitEdgeRefs(maximumRecords, guarded);
			}
		}

		[[nodiscard]] bool supportsNativeRelationshipTypeTokens() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeRelationshipTypeTokens()
				: overlaySnapshot().supportsNativeRelationshipTypeTokens();
		}
		[[nodiscard]] bool supportsNativeRelationshipTypeTokens(std::string_view type) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U
				? derived_snapshot_.supportsNativeRelationshipTypeTokens(type)
				: overlaySnapshot().supportsNativeRelationshipTypeTokens(type);
		}

		[[nodiscard]] std::optional<uint64_t> findRelationshipTypeToken(std::string_view name) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.findRelationshipTypeToken(name)
				: overlaySnapshot().findRelationshipTypeToken(name);
		}

		[[nodiscard]] bool supportsPropertyNonNullProbes() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsPropertyNonNullProbes()
				: overlaySnapshot().supportsPropertyNonNullProbes();
		}

		[[nodiscard]] std::optional<bool> nodePropertyIsNonNull(uint64_t nodeId, std::string_view key) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.nodePropertyIsNonNull(nodeId, key)
				: overlaySnapshot().nodePropertyIsNonNull(nodeId, key);
		}

		[[nodiscard]] std::optional<bool> edgePropertyIsNonNull(uint64_t edgeId, std::string_view key) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.edgePropertyIsNonNull(edgeId, key)
				: overlaySnapshot().edgePropertyIsNonNull(edgeId, key);
		}

		[[nodiscard]] bool supportsNativeAdjacentEdgeTopologyPages() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeAdjacentEdgeTopologyPages()
					: overlaySnapshot().supportsNativeAdjacentEdgeTopologyPages();
		}

		[[nodiscard]] bool supportsNativeAdjacentEdgeRefScans() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeAdjacentEdgeRefScans()
					: overlaySnapshot().supportsNativeAdjacentEdgeRefScans();
		}

		[[nodiscard]] bool supportsAdjacencyReferenceBufferAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsAdjacencyReferenceBufferAdmission()
				: overlaySnapshot().supportsAdjacencyReferenceBufferAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefsWithBufferAdmission(uint64_t nodeId,
			LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords, const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("overlay admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.scanAdjacentEdgeRefsWithBufferAdmission(nodeId, direction, continuation, maximumRecords, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->scanAdjacentEdgeRefsWithBufferAdmission(nodeId, direction, continuation, maximumRecords, guarded);
		}

		[[nodiscard]] LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefs(uint64_t nodeId,
				LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
				size_t maximumRecords) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanAdjacentEdgeRefs(nodeId, direction, continuation, maximumRecords);
			}
			return overlaySnapshot().scanAdjacentEdgeRefs(nodeId, direction, continuation, maximumRecords);
		}

		void visitEdgeTopologies(size_t maximumRecords,
			const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
			requireActive();
			if (!visitor || maximumRecords == 0U) throw std::invalid_argument("overlay topology visitor requires a visitor and positive bounds");
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitEdgeTopologies(maximumRecords, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitEdgeTopologies(maximumRecords, guarded);
			}
		}
		void visitAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
			const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
			requireActive();
			if (!visitor || maximumEdges == 0U) throw std::invalid_argument("overlay topology visitor requires a visitor and positive bounds");
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitAdjacentEdgeTopologies(nodeId, direction, maximumEdges, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitAdjacentEdgeTopologies(nodeId, direction, maximumEdges, guarded);
			}
		}

		[[nodiscard]] bool supportsPropertyTokenResolution() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsPropertyTokenResolution()
				: overlaySnapshot().supportsPropertyTokenResolution();
		}
		[[nodiscard]] bool supportsPropertyPresenceProbes() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsPropertyPresenceProbes()
				: overlaySnapshot().supportsPropertyPresenceProbes();
		}
		[[nodiscard]] std::optional<bool> nodePropertyExists(uint64_t nodeId, std::string_view key) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.nodePropertyExists(nodeId, key)
				: overlaySnapshot().nodePropertyExists(nodeId, key);
		}
		[[nodiscard]] std::optional<bool> edgePropertyExists(uint64_t edgeId, std::string_view key) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.edgePropertyExists(edgeId, key)
				: overlaySnapshot().edgePropertyExists(edgeId, key);
		}

		[[nodiscard]] std::optional<uint32_t> findPropertyToken(std::string_view name) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.findPropertyToken(name);
			}
			return overlaySnapshot().findPropertyToken(name);
		}

		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
				size_t maximumMetadataPages, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages, shape);
			}
			return overlaySnapshot().scanNodesAfter(
					afterNodeId, maximumRecords, maximumMetadataPages, shape);
		}

		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSorted(std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readNodesShapedSorted(nodeIds, shape);
			}
			return overlaySnapshot().readNodesShapedSorted(nodeIds, shape);
		}

		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSorted(std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readEdgesShapedSorted(edgeIds, shape);
			}
			return overlaySnapshot().readEdgesShapedSorted(edgeIds, shape);
		}

		[[nodiscard]] std::vector<PropertyValue>
		readNodePropertiesSorted(std::span<const uint64_t> nodeIds, std::string_view key) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readNodePropertiesSorted(nodeIds, key);
			}
			return overlaySnapshot().readNodePropertiesSorted(nodeIds, key);
		}

		[[nodiscard]] bool supportsShapedPropertyReadAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsShapedPropertyReadAdmission()
					: overlaySnapshot().supportsShapedPropertyReadAdmission();
		}

		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSortedWithAdmission(std::span<const uint64_t> nodeIds, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("shaped Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.readNodesShapedSortedWithAdmission(nodeIds, shape, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->readNodesShapedSortedWithAdmission(nodeIds, shape, guarded);
		}

		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSortedWithAdmission(std::span<const uint64_t> edgeIds, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("shaped Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.readEdgesShapedSortedWithAdmission(edgeIds, shape, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->readEdgesShapedSortedWithAdmission(edgeIds, shape, guarded);
		}

		[[nodiscard]] bool supportsPropertyReadAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsPropertyReadAdmission()
					: overlaySnapshot().supportsPropertyReadAdmission();
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSortedWithAdmission(std::span<const uint64_t> nodeIds,
				std::span<const std::string_view> keys, const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.readNodePropertyColumnsSortedWithAdmission(nodeIds, keys, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->readNodePropertyColumnsSortedWithAdmission(nodeIds, keys, guarded);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSortedWithAdmission(std::span<const uint64_t> edgeIds,
				std::span<const std::string_view> keys, const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.readEdgePropertyColumnsSortedWithAdmission(edgeIds, keys, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->readEdgePropertyColumnsSortedWithAdmission(edgeIds, keys, guarded);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSorted(std::span<const uint64_t> nodeIds, std::span<const std::string_view> keys) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readNodePropertyColumnsSorted(nodeIds, keys);
			}
			return overlaySnapshot().readNodePropertyColumnsSorted(nodeIds, keys);
		}

		[[nodiscard]] std::vector<PropertyValue>
		readEdgePropertiesSorted(std::span<const uint64_t> edgeIds, std::string_view key) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readEdgePropertiesSorted(edgeIds, key);
			}
			return overlaySnapshot().readEdgePropertiesSorted(edgeIds, key);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSorted(std::span<const uint64_t> edgeIds, std::span<const std::string_view> keys) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readEdgePropertyColumnsSorted(edgeIds, keys);
			}
			return overlaySnapshot().readEdgePropertyColumnsSorted(edgeIds, keys);
		}

		[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readNodeShaped(nodeId, shape);
			}
			return overlaySnapshot().readNodeShaped(nodeId, shape);
		}

		[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readEdgeShaped(edgeId, shape);
			}
			return overlaySnapshot().readEdgeShaped(edgeId, shape);
		}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			return readNodeShaped(nodeId, completeNodeReadShape());
		}

		[[nodiscard]] bool supportsNodeExistenceProbes() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNodeExistenceProbes()
					: overlaySnapshot().supportsNodeExistenceProbes();
		}

		[[nodiscard]] std::vector<bool> containsNodesSorted(std::span<const uint64_t> nodeIds) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.containsNodesSorted(nodeIds)
					: overlaySnapshot().containsNodesSorted(nodeIds);
		}

		[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.containsNode(nodeId)
															 : overlaySnapshot().containsNode(nodeId);
		}

		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
			return readEdgeShaped(edgeId, completeEdgeReadShape());
		}

		[[nodiscard]] bool supportsNativeExactNodeCandidates() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeExactNodeCandidates();
		}

		[[nodiscard]] std::optional<uint64_t> nativeSnapshotGeneration() const override {
			requireActive();
			const auto generation = overlay_.baseSnapshot().nativeSnapshotGeneration();
			if (!generation || *generation == 0U) return std::nullopt;
			if (overlay_.logical_revision_ > std::numeric_limits<uint64_t>::max() - *generation)
				throw std::overflow_error("overlay candidate generation overflowed");
			return *generation + overlay_.logical_revision_;
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByExactProperties(
			const std::vector<std::string> &labels, const std::vector<std::pair<std::string, PropertyValue>> &properties,
			size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return overlaySnapshot().findNodeCandidatesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByExactProperties(
			const std::vector<std::string> &labels, const std::vector<std::pair<std::string, PropertyValue>> &properties,
			size_t maximumNodes, size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries, visitor);
		}
		[[nodiscard]] std::optional<bool> visitNodeCandidatesByExactProperties(
			const std::vector<std::string> &labels, const std::vector<std::pair<std::string, PropertyValue>> &properties,
			size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries, maximumCandidateBytes, visitor);
		}

		[[nodiscard]] bool supportsNativeRangeNodeCandidates() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeRangeNodeCandidates();
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByPropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
			size_t maximumNodes, size_t maximumPropertyEntries,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries, visitor);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByPropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
			size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries, maximumCandidateBytes, visitor);
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByPropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
			size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return overlaySnapshot().findNodeCandidatesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
			size_t maximumNodes, size_t maximumPropertyEntries,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries, visitor);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
			size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries, maximumCandidateBytes, visitor);
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByCompositePropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
			size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return overlaySnapshot().findNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries);
		}

		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
		findNodesByExactProperties(const std::vector<std::string> &labels,
											const std::vector<std::pair<std::string, PropertyValue>> &properties,
											size_t maximumNodes, size_t maximumPropertyEntries) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.findNodesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries);
			}
			return overlaySnapshot().findNodesByExactProperties(
					labels, properties, maximumNodes, maximumPropertyEntries);
		}

		void visitNodes(size_t maximumRecords, const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitNodes(maximumRecords, guarded);
				return;
			}
			auto pinned = pinnedOverlaySnapshot();
			pinned->visitNodes(maximumRecords, guarded);
		}

		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																						 size_t maximumMetadataPages) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages);
			}
			return overlaySnapshot().scanNodesAfter(afterNodeId, maximumRecords,
																				maximumMetadataPages);
		}

		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
							  std::optional<LogicalGraphAdjacencyContinuation> continuation,
							  size_t maximumRecords) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanAdjacentEdges(nodeId, direction, std::move(continuation), maximumRecords);
			}
			return overlaySnapshot().scanAdjacentEdges(nodeId, direction, std::move(continuation),
																												 maximumRecords);
		}

		[[nodiscard]] bool supportsAdjacencyReadAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U
				? (derived_snapshot_.supportsAdjacencyReadAdmission() || overlaySnapshot().supportsAdjacencyReadAdmission())
				: overlaySnapshot().supportsAdjacencyReadAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithAdmission(
			uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> &propertyAdmission,
			const std::function<void(size_t)> &bufferAdmission) const override {
			if (!supportsAdjacencyReadAdmission()) throw std::logic_error("overlay combined adjacency admission is unavailable");
			if (!propertyAdmission || !bufferAdmission) throw std::invalid_argument("adjacency admission callbacks must not be empty");
			const auto guardedProperty = guardReadAdmission(propertyAdmission);
			const auto guardedBuffer = guardReadAdmission(bufferAdmission);
			if (overlay_.logical_revision_ == 0U && derived_snapshot_.supportsAdjacencyReadAdmission()) {
				return derived_snapshot_.scanAdjacentEdgesWithAdmission(nodeId, direction,
					std::move(continuation), maximumRecords, guardedProperty, guardedBuffer);
			}
			auto pinned = pinnedOverlaySnapshot();
			return pinned->scanAdjacentEdgesWithAdmission(nodeId, direction,
				std::move(continuation), maximumRecords, guardedProperty, guardedBuffer);
		}

		[[nodiscard]] bool supportsAdjacencyPropertyReadAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U
				? derived_snapshot_.supportsAdjacencyPropertyReadAdmission()
				: overlaySnapshot().supportsAdjacencyPropertyReadAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithPropertyAdmission(
			uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> &admission) const override {
			if (!supportsAdjacencyPropertyReadAdmission()) throw std::logic_error("overlay adjacency Property admission is unavailable");
			if (!admission) throw std::invalid_argument("adjacency Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.scanAdjacentEdgesWithPropertyAdmission(nodeId, direction, std::move(continuation), maximumRecords, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->scanAdjacentEdgesWithPropertyAdmission(nodeId, direction, std::move(continuation), maximumRecords, guarded);
		}

		[[nodiscard]] bool supportsAdjacencyTopologyBufferAdmission() const override {
			requireActive();
			return overlaySnapshot().supportsAdjacencyTopologyBufferAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologiesWithBufferAdmission(uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("overlay admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (!supportsAdjacencyTopologyBufferAdmission()) throw std::logic_error("overlay topology buffer admission is unavailable");
			auto pinned = pinnedOverlaySnapshot();
			return pinned->scanAdjacentEdgeTopologiesWithBufferAdmission(
				nodeId, direction, std::move(continuation), maximumRecords, guarded);
		}

		[[nodiscard]] LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction,
											std::optional<LogicalGraphAdjacencyContinuation> continuation,
											size_t maximumRecords) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanAdjacentEdgeTopologies(nodeId, direction, std::move(continuation), maximumRecords);
			}
			return overlaySnapshot().scanAdjacentEdgeTopologies(nodeId, direction,
																																	 std::move(continuation), maximumRecords);
		}

		void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitEdges(maximumRecords, guarded);
				return;
			}
			auto pinned = pinnedOverlaySnapshot();
			pinned->visitEdges(maximumRecords, guarded);
		}

		void visitAdjacentEdgeRefsForNodes(std::span<const uint64_t> nodeIds,
			LogicalGraphReadDirection direction, size_t maximumEdges,
			const std::function<bool(const EntityRef &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitAdjacentEdgeRefsForNodes(nodeIds, direction, maximumEdges, guarded);
				return;
			}
			auto pinned = pinnedOverlaySnapshot();
			pinned->visitAdjacentEdgeRefsForNodes(nodeIds, direction, maximumEdges, guarded);
		}

		void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
									const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitAdjacentEdges(nodeId, direction, maximumEdges, guarded);
				return;
			}
			auto pinned = pinnedOverlaySnapshot();
			pinned->visitAdjacentEdges(nodeId, direction, maximumEdges, guarded);
		}

		[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
			requireActive();
			return overlay_.overlayFence();
		}
		[[nodiscard]] std::optional<LogicalGraphSchemaIdentity> bindingSchemaIdentity() const override {
			requireActive();
			return overlay_.overlaySchemaIdentity();
		}

		[[nodiscard]] LogicalGraphVectorSearchResult
		searchLogicalVector(const LogicalGraphVectorSearchRequest &request) const override {
			requireActive();
			if (request.cancellation != nullptr) {
				request.cancellation->check();
			}
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.searchLogicalVector(request);
			}
			const std::optional<LogicalGraphVectorIndexDescriptor> index =
					derived_snapshot_.describeLogicalVectorIndex(request);
			if (!index.has_value()) {
				throw std::runtime_error(
						"logical transaction Vector search has no exact overlay evaluator for its pinned generation");
			}
			utils::OwnedStdVector<LogicalGraphVectorSearchHit> hits;
			const auto betterHit = [](const LogicalGraphVectorSearchHit &left, const LogicalGraphVectorSearchHit &right) {
				return left.score == right.score ? left.node_id < right.node_id : left.score < right.score;
			};
			LogicalGraphVectorSearchStats stats;
			stats.used_exact_overlay = true;
			auto graphSnapshot = overlay_.openLogicalGraphReadSnapshot();
			const EntityReadShape shape = derived_snapshot_.exactOverlayCandidateReadShape(*index);
			validateEntityReadShape(shape);
			if (request.maximum_mapping_entries == 0U) {
				throw std::invalid_argument("logical transaction Vector scan requires a positive candidate bound");
			}
			uint64_t after = 0U;
			for (;;) {
				if (request.cancellation != nullptr) request.cancellation->check();
				if (stats.exact_overlay_candidate_scans == request.maximum_mapping_entries) {
					throw std::runtime_error("logical transaction Vector scan exceeded its candidate bound");
				}
				const auto page = graphSnapshot->scanNodesAfter(after,
						std::min(size_t(512U), request.maximum_mapping_entries - stats.exact_overlay_candidate_scans),
						std::numeric_limits<size_t>::max(), shape);
				for (const LogicalGraphNode &node: page.nodes) {
					if (request.cancellation != nullptr) request.cancellation->check();
					++stats.exact_overlay_candidate_scans;
					const auto hit = derived_snapshot_.scoreLogicalVectorExactOverlayCandidate(request, *index, node);
					if (hit.has_value() && request.top_k != 0U) {
						if (hits.size() < request.top_k) {
							hits.push_back(*hit);
							std::push_heap(hits.begin(), hits.end(), betterHit);
						} else if (betterHit(*hit, hits.front())) {
							std::pop_heap(hits.begin(), hits.end(), betterHit);
							hits.back() = *hit;
							std::push_heap(hits.begin(), hits.end(), betterHit);
						}
						debug::PerfTrace::addValue("native_graph.read.overlay_vector_retained_hits", static_cast<int64_t>(hits.size()));
					}
					after = node.node_id;
				}
				if (!page.may_have_more) break;
			}

			if (request.cancellation != nullptr) {
				request.cancellation->check();
			}
			std::sort_heap(hits.begin(), hits.end(), betterHit);
			return {std::move(hits), stats};
		}

	private:
		[[nodiscard]] std::shared_ptr<LogicalGraphReadSnapshot> pinnedOverlaySnapshot() const {
			static_cast<void>(overlaySnapshot());
			return overlay_snapshot_;
		}

		[[nodiscard]] LogicalGraphReadSnapshot &overlaySnapshot() const {
			requireActive();
			if (!overlay_snapshot_ || overlay_snapshot_revision_ != overlay_.logical_revision_) {
				overlay_snapshot_ = overlay_.openLogicalGraphReadSnapshot();
				overlay_snapshot_revision_ = overlay_.logical_revision_;
			}
			return *overlay_snapshot_;
		}


		void requireActive() const {
			if (overlay_.transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
				throw std::logic_error("logical transaction Vector snapshot is no longer active");
			}
		}

		const LogicalGraphTransactionOverlay &overlay_;
		LogicalGraphVectorSearchSnapshot &derived_snapshot_;
		// Continuation mappings belong to one logical revision of this wrapper.
		mutable std::shared_ptr<LogicalGraphReadSnapshot> overlay_snapshot_;
		mutable uint64_t overlay_snapshot_revision_ = 0U;
	};

