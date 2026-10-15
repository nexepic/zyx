/**
 * @file PageBackedLogicalGraphReadAdapter.cpp
 * @brief Query-facing logical records materialized from native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graph/debug/PerfTrace.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/storage/detail/ScopedPropertyBuffers.hpp"
#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void throwLogicalAdapterError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		// Admission may throw during allocation. Deallocation never invokes user code.
		// The caller reconciles released temporary storage after materialization.
		class AdmittedTemporaryResource final : public std::pmr::memory_resource {
		public:
            explicit AdmittedTemporaryResource(const std::function<void(size_t)> &admission,
                    std::shared_ptr<std::pmr::memory_resource> fallbackOwner = {})
                : admission_(admission), memory_owner_(admission ? nullptr :
                    (utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : std::move(fallbackOwner))),
                  upstream_(memory_owner_ ? memory_owner_.get() : std::pmr::new_delete_resource()) {}
		private:
			void *do_allocate(size_t bytes, size_t alignment) override {
				if (bytes > std::numeric_limits<size_t>::max() - retained_) throw std::overflow_error("temporary allocation size overflowed");
				if (admission_) admission_(retained_ + bytes);
				void *result = upstream_->allocate(bytes, alignment);
				retained_ += bytes;
				return result;
			}
			void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
				upstream_->deallocate(pointer, bytes, alignment);
				retained_ -= bytes;
			}
			bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
            const std::function<void(size_t)> &admission_;
            std::shared_ptr<std::pmr::memory_resource> memory_owner_;
            size_t retained_ = 0U;
            std::pmr::memory_resource *upstream_;
		};

		void validateNamedPropertyColumnRequest(std::span<const uint64_t> ids,
			std::span<const std::string_view> keys) {
			uint64_t previous = 0U;
			for (const uint64_t id: ids) {
				if (id == 0U || id <= previous)
					throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
						"named Property columns require strictly increasing nonzero Entity IDs");
				previous = id;
			}
			if (std::ranges::any_of(keys, [](std::string_view key) { return key.empty(); }))
				throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER, "named Property columns require non-empty keys");
		}

		std::vector<std::vector<PropertyValue>> makeEmptyPropertyColumns(size_t columns, size_t rows) {
			std::vector<std::vector<PropertyValue>> result;
			result.reserve(columns);
			for (size_t column = 0U; column < columns; ++column) result.emplace_back(rows);
			return result;
		}

		[[nodiscard]] PageBackedGraphReadDirection pageDirection(LogicalGraphReadDirection direction) {
			switch (direction) {
				case LogicalGraphReadDirection::LGRD_OUTGOING:
					return PageBackedGraphReadDirection::PBGRD_OUTGOING;
				case LogicalGraphReadDirection::LGRD_INCOMING:
					return PageBackedGraphReadDirection::PBGRD_INCOMING;
			}
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									 "page-backed logical graph adapter has an unknown read direction");
		}

		[[nodiscard]] std::optional<PageBackedGraphAdjacencyContinuation>
		pageContinuation(const std::optional<LogicalGraphAdjacencyContinuation> &continuation) {
			if (!continuation.has_value()) {
				return std::nullopt;
			}
			return PageBackedGraphAdjacencyContinuation{continuation->previous_edge_id, continuation->next_edge_id};
		}

		[[nodiscard]] std::optional<LogicalGraphAdjacencyContinuation>
		logicalContinuation(const std::optional<PageBackedGraphAdjacencyContinuation> &continuation) {
			if (!continuation.has_value()) {
				return std::nullopt;
			}
			return LogicalGraphAdjacencyContinuation{continuation->previous_edge_id, continuation->next_edge_id};
		}

		[[nodiscard]] uint64_t elapsedNanoseconds(const std::chrono::steady_clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
		}

		NativeReadSession &activeSession(const MetadataSnapshotPin &snapshot, NativeReadSession *session,
																			 std::optional<NativeReadSession> &localSession) {
			return selectNativeReadSession(snapshot, session, localSession);
		}

	} // namespace

	PageBackedLogicalGraphAdjacencyCursor::PageBackedLogicalGraphAdjacencyCursor(
			PageBackedLogicalGraphAdjacencyCursor &&other) noexcept :
			adapter_(std::exchange(other.adapter_, nullptr)), native_(std::move(other.native_)),
			session_(std::exchange(other.session_, nullptr)), owned_session_(std::move(other.owned_session_)) {
		other.native_.reset();
	}

	PageBackedLogicalGraphAdjacencyCursor &
	PageBackedLogicalGraphAdjacencyCursor::operator=(PageBackedLogicalGraphAdjacencyCursor &&other) noexcept {
		if (this != &other) {
			adapter_ = std::exchange(other.adapter_, nullptr);
			native_ = std::move(other.native_);
			session_ = std::exchange(other.session_, nullptr);
			owned_session_ = std::move(other.owned_session_);
			other.native_.reset();
		}
		return *this;
	}

	LogicalGraphAdjacencyPage PageBackedLogicalGraphAdjacencyCursor::next(
			const MetadataSnapshotPin &snapshot, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords) {
		// The private constructor always binds a live adapter and native cursor together.
		if (adapter_ == nullptr || !native_.has_value() || session_ == nullptr) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									 "page-backed logical adjacency cursor has no native scan state");
		}
		const PageBackedGraphAdjacencyPage raw = native_->next(snapshot, pageContinuation(continuation), maximumRecords);
		LogicalGraphAdjacencyPage result;
		result.edges.reserve(raw.edges.size());
		for (const PageBackedGraphEdge &edge: raw.edges) {
			result.edges.push_back(adapter_->materializeEdge(snapshot, edge, *session_));
		}
		result.continuation = logicalContinuation(raw.continuation);
		return result;
	}

	PageBackedLogicalGraphReadAdapter::PageBackedLogicalGraphReadAdapter(PageBackedGraphReadAdapter &pages,
																								 const PageBackedGraphNameResolver &names) :
		pages_(pages), names_(names) {}

	std::string PageBackedLogicalGraphReadAdapter::resolveNodeLabelName(const MetadataSnapshotPin &snapshot,
																			uint64_t tokenId, NativeReadSession &session) const {
		if (auto cached = session.tokenName(NativeReadSession::TokenNameKind::TNK_NODE_LABEL, tokenId);
            cached.has_value()) {
            return std::move(*cached);
		}
		std::string resolved = names_.resolveNodeLabelToken(snapshot, tokenId);
		if (!resolved.empty()) {
			session.putTokenName(NativeReadSession::TokenNameKind::TNK_NODE_LABEL, tokenId, resolved);
		}
		return resolved;
	}

	std::string PageBackedLogicalGraphReadAdapter::resolveRelationshipTypeName(const MetadataSnapshotPin &snapshot,
																								 uint64_t tokenId, NativeReadSession &session, const std::function<void(size_t)> &copyAdmission) const {
		if (auto cached = session.tokenName(NativeReadSession::TokenNameKind::TNK_RELATIONSHIP_TYPE, tokenId, copyAdmission);
			cached.has_value()) {
			return std::move(*cached);
		}
		std::string resolved = names_.resolveRelationshipTypeTokenWithNameAdmission(snapshot, tokenId, copyAdmission);
		if (copyAdmission) copyAdmission(resolved.capacity());
		if (!resolved.empty()) {
			session.putTokenName(NativeReadSession::TokenNameKind::TNK_RELATIONSHIP_TYPE, tokenId, resolved);
		}
		return resolved;
	}

	std::string PageBackedLogicalGraphReadAdapter::resolvePropertyKeyName(const MetadataSnapshotPin &snapshot,
																			 uint32_t tokenId, NativeReadSession &session, const std::function<void(size_t)> &copyAdmission) const {
		if (auto cached = session.tokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY, tokenId, copyAdmission);
			cached.has_value()) {
			return std::move(*cached);
		}
		std::string resolved = names_.resolvePropertyKeyWithNameAdmission(snapshot, tokenId, copyAdmission);
		if (copyAdmission) copyAdmission(resolved.capacity());
		if (!resolved.empty()) {
			session.putTokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY, tokenId, resolved);
		}
		return resolved;
	}

	std::optional<std::vector<uint64_t>> PageBackedLogicalGraphReadAdapter::resolveNodeLabelTokens(
			const MetadataSnapshotPin &snapshot, const std::span<const std::string> labels, NativeReadSession &session) const {
		std::vector<uint64_t> tokens;
		tokens.reserve(labels.size());
		for (const std::string &label: labels) {
			if (label.empty()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									  "page-backed Node label test requires non-empty names");
			}
			const std::optional<std::optional<uint64_t>> cached = session.nodeLabelToken(label);
			const std::optional<uint64_t> token =
					cached.has_value() ? *cached : names_.resolveNodeLabelName(snapshot, label);
			if (!cached.has_value()) {
				session.putNodeLabelToken(label, token);
			}
			if (!token.has_value()) {
				return std::nullopt;
			}
			if (*token == 0U) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed Node label test resolved an invalid token");
			}
			tokens.push_back(*token);
		}
		std::ranges::sort(tokens);
		if (std::ranges::adjacent_find(tokens) != tokens.end()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed Node label test resolved duplicate tokens");
		}
		return tokens;
	}

	LogicalGraphNode PageBackedLogicalGraphReadAdapter::materializeNode(
			const MetadataSnapshotPin &snapshot, const PageBackedGraphNode &node, NativeReadSession &session) const {
		return materializeNodeImpl(snapshot, node, session);
	}

	LogicalGraphNode PageBackedLogicalGraphReadAdapter::materializeNode(
			const MetadataSnapshotPin &snapshot, PageBackedGraphNode &&node, NativeReadSession &session) const {
		return materializeNodeImpl(snapshot, node, session);
	}

	template<typename RawNode>
	LogicalGraphNode PageBackedLogicalGraphReadAdapter::materializeNodeImpl(
			const MetadataSnapshotPin &snapshot, RawNode &node, NativeReadSession &session) const {
		// Entity Map publication rejects zero Node IDs before the native read adapter materializes.
		if (node.node_id == 0U) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									 "page-backed logical graph adapter received a Node with no logical ID");
		}
		LogicalGraphNode result;
		result.node_id = node.node_id;
        detail::admitScopedPropertyVectorGrowth(result.labels, node.label_token_ids.size());
        result.labels.reserve(node.label_token_ids.size());
        // Output reserve keeps SSO and external string storage stable while
        // these duplicate-detection tables borrow its keys.
        const auto nameMemory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
            session.memoryOwner();
        auto *const nameResource = nameMemory ? nameMemory.get() : std::pmr::new_delete_resource();
        std::pmr::unordered_set<std::string_view> labelNames(nameResource);
        labelNames.reserve(node.label_token_ids.size());
		for (const uint64_t tokenId: node.label_token_ids) {
			// NodeEntityRecordCodec rejects zero inline label tokens.
			if (tokenId == 0U) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter received a zero Node label token");
			}
			std::string name = resolveNodeLabelName(snapshot, tokenId, session);
			if (name.empty() || labelNames.contains(name)) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter resolved invalid or duplicate Node labels");
			}
            result.labels.push_back(std::move(name));
            labelNames.insert(result.labels.back());
        }
        detail::admitScopedPropertyVectorGrowth(result.properties, node.properties.size());
        result.properties.reserve(node.properties.size());
        std::pmr::unordered_set<std::string_view> propertyNames(nameResource);
        propertyNames.reserve(node.properties.size());
		for (auto &property: node.properties) {
			// Property-root decoding rejects a zero property token before this adapter.
			if (property.key_token == 0U) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter received a zero Node Property token");
			}
			std::string key = resolvePropertyKeyName(snapshot, property.key_token, session);
			if (key.empty() || propertyNames.contains(key)) {
				throwLogicalAdapterError(
						FormatError::FERR_INVALID_STATE,
						"page-backed logical graph adapter resolved invalid or duplicate Node Properties");
			}
			// Complete reads are also a source for later selected-column reads in
			// the same snapshot.  Publish the authenticated value into the bounded
			// session cache so a subsequent predicate/projection does not reopen the
			// Property directory for the same token.
			session.putProperty(false, node.node_id, property.key_token, property.value);
			if constexpr (std::is_const_v<RawNode>) {
				result.properties.push_back({std::move(key), property.value});
			} else {
				result.properties.push_back({std::move(key), std::move(property.value)});
            }
            propertyNames.insert(result.properties.back().key);
        }
        debug::PerfTrace::addValue("native_graph.read.full_materializations", 1);
		return result;
	}

	LogicalGraphEdge PageBackedLogicalGraphReadAdapter::materializeEdge(
			const MetadataSnapshotPin &snapshot, const PageBackedGraphEdge &edge, NativeReadSession &session) const {
		return materializeEdgeImpl(snapshot, edge, session);
	}

	LogicalGraphEdge PageBackedLogicalGraphReadAdapter::materializeEdge(
			const MetadataSnapshotPin &snapshot, PageBackedGraphEdge &&edge, NativeReadSession &session) const {
		return materializeEdgeImpl(snapshot, edge, session);
	}

	template<typename RawEdge>
	LogicalGraphEdge PageBackedLogicalGraphReadAdapter::materializeEdgeImpl(
			const MetadataSnapshotPin &snapshot, RawEdge &edge, NativeReadSession &session,
			const std::function<void(size_t)> &typeNameAdmission,
			const std::function<void(size_t)> &propertySlotsAdmission,
			const std::function<void(size_t)> &temporaryAdmission) const {
		// Entity Map and EdgeEntityRecordCodec authenticate these three durable identities.
		if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
			throwLogicalAdapterError(
					FormatError::FERR_INVALID_STATE,
					"page-backed logical graph adapter received an Edge with invalid identity or endpoints");
		}
		LogicalGraphEdge result;
		result.edge_id = edge.edge_id;
		result.source_node_id = edge.source_node_id;
		result.target_node_id = edge.target_node_id;
		if (edge.type_id != 0U) {
			result.type = resolveRelationshipTypeName(snapshot, edge.type_id, session, typeNameAdmission);
			if (result.type.empty()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter resolved an empty Edge type name");
			}
		}
		size_t retainedNameBytes = result.type.empty() ? 0U : result.type.capacity();
		const auto admitPropertySlots = [&](size_t count) {
			if (count > std::numeric_limits<size_t>::max() / sizeof(LogicalGraphPropertyEntry)) {
				throwLogicalAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "logical Edge Property slots overflowed");
			}
			if (propertySlotsAdmission) propertySlotsAdmission(count * sizeof(LogicalGraphPropertyEntry));
		};
        admitPropertySlots(edge.properties.size());
        if (!propertySlotsAdmission) detail::admitScopedPropertyVectorGrowth(result.properties, edge.properties.size());
        result.properties.reserve(edge.properties.size());
		admitPropertySlots(result.properties.capacity());
		// reserve keeps output keys stable while the duplicate set borrows them.
		AdmittedTemporaryResource propertyNameMemory(temporaryAdmission, session.memoryOwner());
		std::pmr::unordered_set<std::string_view> propertyNames(&propertyNameMemory);
		propertyNames.reserve(edge.properties.size());
		for (auto &property: edge.properties) {
			// Property-root decoding rejects a zero property token before this adapter.
			if (property.key_token == 0U) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter received a zero Edge Property token");
			}
			std::string key = resolvePropertyKeyName(snapshot, property.key_token, session, [&](size_t bytes) {
				if (bytes > std::numeric_limits<size_t>::max() - retainedNameBytes) {
					throwLogicalAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "Edge Property name size overflowed");
				}
				if (typeNameAdmission) typeNameAdmission(retainedNameBytes + bytes);
			});
			if (key.capacity() > std::numeric_limits<size_t>::max() - retainedNameBytes) {
				throwLogicalAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "retained Edge Property names overflowed");
			}
			retainedNameBytes += key.capacity();
			if (typeNameAdmission) typeNameAdmission(retainedNameBytes);
			if (key.empty() || propertyNames.contains(key)) {
				throwLogicalAdapterError(
						FormatError::FERR_INVALID_STATE,
						"page-backed logical graph adapter resolved invalid or duplicate Edge Properties");
			}
			session.putProperty(true, edge.edge_id, property.key_token, property.value);
			if constexpr (std::is_const_v<RawEdge>) {
				result.properties.push_back({std::move(key), property.value});
			} else {
				result.properties.push_back({std::move(key), std::move(property.value)});
			}
			propertyNames.insert(result.properties.back().key);
		}
		debug::PerfTrace::addValue("native_graph.read.full_materializations", 1);
		return result;
	}

	LogicalGraphNode PageBackedLogicalGraphReadAdapter::readNode(const MetadataSnapshotPin &snapshot,
																																				 uint64_t nodeId, NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		return materializeNode(snapshot, pages_.readNode(snapshot, nodeId, &active), active);
	}

	LogicalGraphNode PageBackedLogicalGraphReadAdapter::readNodeShaped(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, const EntityReadShape &shape,
			NativeReadSession *session) const {
		validateEntityReadShape(shape);
		if (shape.all_properties) {
			std::optional<NativeReadSession> localSession;
			NativeReadSession &active = activeSession(snapshot, session, localSession);
			PageBackedGraphNode raw;
			raw.node_id = nodeId;
			if (shape.include_labels) {
				raw.label_token_ids = pages_.readNodeLabelTokens(snapshot, nodeId, &active);
			}
			raw.properties = pages_.readNodeProperties(snapshot, nodeId, &active);
			return materializeNode(snapshot, std::move(raw), active);
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		static_cast<void>(pages_.readNodeRecord(snapshot, nodeId, &active));
		LogicalGraphNode result;
		result.node_id = nodeId;
		if (shape.include_labels) {
			for (const uint64_t token: pages_.readNodeLabelTokens(snapshot, nodeId, &active)) {
				const std::string label = resolveNodeLabelName(snapshot, token, active);
				if (label.empty()) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed shaped Node read resolved an empty label");
				}
				result.labels.push_back(label);
			}
		}
		if (!shape.property_tokens.empty()) {
			const std::array<uint64_t, 1U> ids{nodeId};
			auto columns =
				pages_.readNodePropertyColumnsSorted(snapshot, ids, shape.property_tokens, &active);
			if (columns.size() != shape.property_tokens.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed shaped Node read changed Property column cardinality");
			}
			for (size_t index = 0U; index < columns.size(); ++index) {
				if (columns[index].size() != 1U) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed shaped Node read changed Property row cardinality");
				}
				const std::string key = resolvePropertyKeyName(snapshot, shape.property_tokens[index], active);
				if (key.empty()) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed shaped Node read resolved an empty Property key");
				}
				if (columns[index][0].has_value()) {
					result.properties.push_back({key, std::move(*columns[index][0])});
				}
			}
		}
		return result;
	}

	std::vector<LogicalGraphNode> PageBackedLogicalGraphReadAdapter::readNodesShapedSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const EntityReadShape &shape, NativeReadSession *session, const SelectedPropertyReadAdmission *admission) const {
		validateEntityReadShape(shape);
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		if (nodeIds.empty()) {
			return {};
		}
		const std::vector<NodeEntityRecord> records = pages_.readNodeRecordsSorted(snapshot, nodeIds, &active);
		if (records.size() != nodeIds.size()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								 "page-backed shaped Node batch changed record cardinality");
		}
		if (shape.all_properties) {
			std::vector<LogicalGraphNode> result;
			result.reserve(nodeIds.size());
			auto propertyRoots = pages_.readNodePropertiesSorted(snapshot, nodeIds, &active, admission);
			auto labelTokens = shape.include_labels
					? pages_.readNodeLabelTokensSorted(snapshot, nodeIds, &active)
					: std::vector<std::vector<uint64_t>>{};
			if (shape.include_labels && labelTokens.size() != nodeIds.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
						"page-backed shaped Node batch changed label cardinality");
			}
			if (propertyRoots.size() != nodeIds.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
						"page-backed shaped Node batch changed complete Property cardinality");
			}
			for (size_t index = 0U; index < nodeIds.size(); ++index) {
				// Use the already-resolved fixed record instead of calling the scalar
				// shaped path again. The session cache is bounded, so a large batch
				// must not turn into one Entity-Map lookup per row after eviction.
				PageBackedGraphNode raw;
				raw.node_id = nodeIds[index];
				if (shape.include_labels) {
					raw.label_token_ids = std::move(labelTokens[index]);
				}
				raw.properties = std::move(propertyRoots[index]);
				result.push_back(materializeNode(snapshot, std::move(raw), active));
			}
			return result;
		}
		std::vector<std::vector<std::optional<PropertyValue>>> columns;
		const auto labelTokens = shape.include_labels
				? pages_.readNodeLabelTokensSorted(snapshot, nodeIds, &active)
				: std::vector<std::vector<uint64_t>>{};
		if (shape.include_labels && labelTokens.size() != nodeIds.size()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed shaped Node batch changed label cardinality");
		}
		if (!shape.property_tokens.empty()) {
			columns = pages_.readNodePropertyColumnsSorted(snapshot, nodeIds, shape.property_tokens, &active, admission);
			if (columns.size() != shape.property_tokens.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									 "page-backed shaped Node batch changed Property column cardinality");
			}
			for (const auto &column: columns) {
				if (column.size() != nodeIds.size()) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									 "page-backed shaped Node batch changed Property row cardinality");
				}
			}
		}
		std::vector<std::string> propertyNames;
		propertyNames.reserve(shape.property_tokens.size());
		for (const uint32_t token: shape.property_tokens) {
			propertyNames.push_back(resolvePropertyKeyName(snapshot, token, active));
			if (propertyNames.back().empty()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								 "page-backed shaped Node batch resolved an empty Property key");
			}
		}
		std::vector<LogicalGraphNode> result;
		result.reserve(nodeIds.size());
		for (size_t row = 0U; row < nodeIds.size(); ++row) {
			LogicalGraphNode node;
			node.node_id = nodeIds[row];
			if (shape.include_labels) {
				for (const uint64_t token: labelTokens[row]) {
					const std::string label = resolveNodeLabelName(snapshot, token, active);
					if (label.empty()) {
						throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed shaped Node batch resolved an empty label");
					}
					node.labels.push_back(label);
				}
			}
			for (size_t column = 0U; column < shape.property_tokens.size(); ++column) {
				auto &value = columns[column][row];
				if (value.has_value()) {
					node.properties.push_back({propertyNames[column], std::move(*value)});
				}
			}
			result.push_back(std::move(node));
		}
		return result;
	}

	LogicalGraphNode PageBackedLogicalGraphReadAdapter::materializeNativeNode(
			const MetadataSnapshotPin &snapshot, const PageBackedGraphNode &node, NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		return materializeNode(snapshot, node, activeSession(snapshot, session, localSession));
	}

	LogicalGraphEdge PageBackedLogicalGraphReadAdapter::readEdge(const MetadataSnapshotPin &snapshot,
																				 uint64_t edgeId, NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		return materializeEdge(snapshot, pages_.readEdge(snapshot, edgeId, &active), active);
	}

	LogicalGraphEdge PageBackedLogicalGraphReadAdapter::readEdgeShaped(
			const MetadataSnapshotPin &snapshot, const uint64_t edgeId, const EntityReadShape &shape,
			NativeReadSession *session) const {
		validateEntityReadShape(shape);
		if (shape.all_properties) {
			std::optional<NativeReadSession> localSession;
			NativeReadSession &active = activeSession(snapshot, session, localSession);
			PageBackedGraphEdge raw = pages_.readEdge(snapshot, edgeId, &active);
			if (!shape.include_type) {
				raw.type_id = 0U;
			}
			return materializeEdge(snapshot, std::move(raw), active);
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const EdgeEntityRecord record = pages_.readEdgeTopology(snapshot, edgeId, &active);
		LogicalGraphEdge result{edgeId, record.source_node_id, record.target_node_id, {}, {}};
		if (shape.include_type) {
			result.type = resolveRelationshipTypeName(snapshot, record.type_id, active);
		}
		if (!shape.property_tokens.empty()) {
			const std::array<uint64_t, 1U> ids{edgeId};
			auto columns =
				pages_.readEdgePropertyColumnsSorted(snapshot, ids, shape.property_tokens, &active);
			if (columns.size() != shape.property_tokens.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed shaped Edge read changed Property column cardinality");
			}
			for (size_t index = 0U; index < columns.size(); ++index) {
				if (columns[index].size() != 1U) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed shaped Edge read changed Property row cardinality");
				}
				const std::string key = resolvePropertyKeyName(snapshot, shape.property_tokens[index], active);
				if (key.empty()) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed shaped Edge read resolved an empty Property key");
				}
				if (columns[index][0].has_value()) {
					result.properties.push_back({key, std::move(*columns[index][0])});
				}
			}
		}
		return result;
	}

	std::vector<LogicalGraphEdge> PageBackedLogicalGraphReadAdapter::readEdgesShapedSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			const EntityReadShape &shape, NativeReadSession *session, const SelectedPropertyReadAdmission *admission) const {
		validateEntityReadShape(shape);
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		if (edgeIds.empty()) {
			return {};
		}
		const std::vector<EdgeEntityRecord> records = pages_.readEdgeTopologiesSorted(snapshot, edgeIds, &active);
		if (records.size() != edgeIds.size()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								 "page-backed shaped Edge batch changed record cardinality");
		}
		if (shape.all_properties) {
			auto propertyRoots = pages_.readEdgePropertiesSorted(snapshot, edgeIds, &active, admission);
			if (propertyRoots.size() != edgeIds.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
						"page-backed shaped Edge batch changed complete Property cardinality");
			}
			std::vector<LogicalGraphEdge> result;
			result.reserve(edgeIds.size());
			for (size_t index = 0U; index < edgeIds.size(); ++index) {
				// The topology batch above already authenticated every fixed Edge
				// record. Materialize directly from that aligned buffer so cache FIFO
				// eviction cannot reintroduce scalar Entity lookups.
				PageBackedGraphEdge raw;
				raw.edge_id = edgeIds[index];
				raw.source_node_id = records[index].source_node_id;
				raw.target_node_id = records[index].target_node_id;
				raw.type_id = shape.include_type ? records[index].type_id : 0U;
				raw.properties = std::move(propertyRoots[index]);
				result.push_back(materializeEdge(snapshot, std::move(raw), active));
			}
			return result;
		}
		std::vector<std::vector<std::optional<PropertyValue>>> columns;
		if (!shape.property_tokens.empty()) {
			columns = pages_.readEdgePropertyColumnsSorted(snapshot, edgeIds, shape.property_tokens, &active, admission);
			if (columns.size() != shape.property_tokens.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									 "page-backed shaped Edge batch changed Property column cardinality");
			}
			for (const auto &column: columns) {
				if (column.size() != edgeIds.size()) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									 "page-backed shaped Edge batch changed Property row cardinality");
				}
			}
		}
		std::vector<std::string> propertyNames;
		propertyNames.reserve(shape.property_tokens.size());
		for (const uint32_t token: shape.property_tokens) {
			propertyNames.push_back(resolvePropertyKeyName(snapshot, token, active));
			if (propertyNames.back().empty()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								 "page-backed shaped Edge batch resolved an empty Property key");
			}
		}
		std::vector<LogicalGraphEdge> result;
		result.reserve(edgeIds.size());
		const bool traceValues = debug::PerfTrace::areValuesEnabled();
		size_t selectedPropertyValues = 0U;
		size_t selectedPropertyPayloadBytes = 0U;
		for (size_t row = 0U; row < edgeIds.size(); ++row) {
			const EdgeEntityRecord &record = records[row];
			LogicalGraphEdge edge{edgeIds[row], record.source_node_id, record.target_node_id, {}, {}};
			if (shape.include_type && record.type_id != 0U) {
				edge.type = resolveRelationshipTypeName(snapshot, record.type_id, active);
				if (edge.type.empty()) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed shaped Edge batch resolved an empty relationship type");
				}
			}
			for (size_t column = 0U; column < shape.property_tokens.size(); ++column) {
				auto &value = columns[column][row];
				if (value.has_value()) {
					if (traceValues) {
						++selectedPropertyValues;
						const size_t valueBytes = property_utils::getPropertyValueSize(*value);
						if (valueBytes > std::numeric_limits<size_t>::max() - selectedPropertyPayloadBytes) {
							// Instrumentation cannot change query semantics. Saturate an
							// impractically large metric instead of rejecting a valid value.
							selectedPropertyPayloadBytes = std::numeric_limits<size_t>::max();
						} else {
							selectedPropertyPayloadBytes += valueBytes;
						}
					}
					edge.properties.push_back({propertyNames[column], std::move(*value)});
				}
			}
			result.push_back(std::move(edge));
		}
		if (traceValues) {
			debug::PerfTrace::addValue("native_graph.read.edge_selected_property_output_values",
					selectedPropertyValues > static_cast<size_t>(std::numeric_limits<int64_t>::max())
							? std::numeric_limits<int64_t>::max()
							: static_cast<int64_t>(selectedPropertyValues));
			debug::PerfTrace::addValue("native_graph.read.edge_selected_property_output_payload_bytes",
					selectedPropertyPayloadBytes > static_cast<size_t>(std::numeric_limits<int64_t>::max())
							? std::numeric_limits<int64_t>::max()
							: static_cast<int64_t>(selectedPropertyPayloadBytes));
		}
		return result;
	}

	PropertyValue PageBackedLogicalGraphReadAdapter::readNodeProperty(const MetadataSnapshotPin &snapshot,
															 uint64_t nodeId, const std::string_view key,
															 NativeReadSession *session) const {
		if (key.empty()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
											 "page-backed Node Property read requires a non-empty key");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const std::optional<std::optional<uint32_t>> cached = active.propertyToken(key);
		const std::optional<uint32_t> token = cached.has_value() ? *cached : names_.resolvePropertyToken(snapshot, key);
		if (!cached.has_value()) {
			active.putPropertyToken(key, token);
		}
		if (!token.has_value() || *token == 0U) {
			return PropertyValue();
		}
		const auto value = pages_.readNodeProperty(snapshot, nodeId, *token, &active);
		return value.value_or(PropertyValue());
	}

	bool PageBackedLogicalGraphReadAdapter::nodePropertyExists(const MetadataSnapshotPin &snapshot,
																			 const uint64_t nodeId, const std::string_view key,
																			 NativeReadSession *session, const bool requireNonNull) const {
		if (key.empty()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									"page-backed Node Property presence read requires a non-empty key");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const std::optional<std::optional<uint32_t>> cached = active.propertyToken(key);
		const std::optional<uint32_t> token = cached.has_value() ? *cached : names_.resolvePropertyToken(snapshot, key);
		if (!cached.has_value()) {
			active.putPropertyToken(key, token);
		}
		return token.has_value() && *token != 0U && pages_.nodePropertyExists(snapshot, nodeId, *token, &active, requireNonNull);
	}

	std::vector<PropertyValue> PageBackedLogicalGraphReadAdapter::readNodePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds, const std::string_view key,
			NativeReadSession *session) const {
		if (key.empty()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									  "page-backed sorted Node Property read requires a non-empty key");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const std::optional<std::optional<uint32_t>> cached = active.propertyToken(key);
		const std::optional<uint32_t> token = cached.has_value() ? *cached : names_.resolvePropertyToken(snapshot, key);
		if (!cached.has_value()) {
			active.putPropertyToken(key, token);
		}

		std::vector<PropertyValue> result(nodeIds.size());
		if (!token.has_value() || *token == 0U) {
			return result;
		}
		const std::vector<std::optional<PropertyValue>> values =
				pages_.readNodePropertiesSorted(snapshot, nodeIds, *token, &active);
		if (values.size() != result.size()) { // ZYX_COV_EXCL_LINE: the native adapter always preserves input arity.
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed sorted Node Property read changed result cardinality");
		}
		for (size_t index = 0U; index < values.size(); ++index) {
			result[index] = values[index].value_or(PropertyValue());
		}
		return result;
	}

	std::vector<std::vector<PropertyValue>> PageBackedLogicalGraphReadAdapter::readNodePropertyColumnsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const std::span<const std::string_view> keys, NativeReadSession *session,
			const SelectedPropertyReadAdmission *admission) const {
		validateNamedPropertyColumnRequest(nodeIds, keys);
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		auto result = makeEmptyPropertyColumns(keys.size(), nodeIds.size());
		std::vector<std::pair<uint32_t, size_t>> resolved;
		resolved.reserve(keys.size());
		for (size_t index = 0U; index < keys.size(); ++index) {
			const std::optional<std::optional<uint32_t>> cached = active.propertyToken(keys[index]);
			const std::optional<uint32_t> token =
					cached.has_value() ? *cached : names_.resolvePropertyToken(snapshot, keys[index]);
			if (!cached.has_value()) {
				active.putPropertyToken(keys[index], token);
			}
			if (token.has_value() && *token != 0U) {
				resolved.emplace_back(*token, index);
			}
		}
		std::ranges::sort(resolved, {}, &std::pair<uint32_t, size_t>::first);
		std::vector<uint32_t> tokens;
		size_t uniqueTokens = 0U;
		uint32_t previousToken = 0U;
		for (const auto &[token, outputIndex]: resolved) {
			if (token != previousToken) { ++uniqueTokens; previousToken = token; }
		}
		tokens.reserve(uniqueTokens);
		for (const auto &[token, outputIndex]: resolved)
			if (tokens.empty() || tokens.back() != token) tokens.push_back(token);
		if (tokens.empty()) {
			return result;
		}
		std::vector<std::vector<std::optional<PropertyValue>>> values =
				pages_.readNodePropertyColumnsSorted(snapshot, nodeIds, tokens, &active, admission);
		if (values.size() != tokens.size()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed Node Property column read changed column cardinality");
		}
		size_t resolvedIndex = 0U;
		for (size_t tokenIndex = 0U; tokenIndex < values.size(); ++tokenIndex) {
			if (values[tokenIndex].size() != nodeIds.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed Node Property column read changed row cardinality");
			}
			size_t groupEnd = resolvedIndex;
			while (groupEnd < resolved.size() && resolved[groupEnd].first == tokens[tokenIndex]) ++groupEnd;
			for (; resolvedIndex < groupEnd; ++resolvedIndex) {
				const size_t outputIndex = resolved[resolvedIndex].second;
				const bool lastAlias = resolvedIndex + 1U == groupEnd;
				for (size_t row = 0U; row < nodeIds.size(); ++row) {
					auto &value = values[tokenIndex][row];
					// Duplicate requested keys need independent values; the last alias
					// can take the decoded payload without another copy.
					if (admission != nullptr && value.has_value() && !lastAlias) {
						const size_t encodedBytes = CanonicalPropertyValueCodec::encodedSize(*value);
						if (encodedBytes > std::numeric_limits<size_t>::max() - sizeof(uint32_t)) {
							throw std::overflow_error("logical Property entry size overflowed");
						}
						(*admission)(encodedBytes + sizeof(uint32_t));
					}
					result[outputIndex][row] = !value.has_value() ? PropertyValue()
							: lastAlias ? std::move(*value) : *value;
				}
			}
		}
		return result;
	}

	PropertyValue PageBackedLogicalGraphReadAdapter::readEdgeProperty(const MetadataSnapshotPin &snapshot,
																			 uint64_t edgeId, const std::string_view key,
																			 NativeReadSession *session) const {
		if (key.empty()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
											 "page-backed Edge Property read requires a non-empty key");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const std::optional<std::optional<uint32_t>> cached = active.propertyToken(key);
		const std::optional<uint32_t> token = cached.has_value() ? *cached : names_.resolvePropertyToken(snapshot, key);
		if (!cached.has_value()) {
			active.putPropertyToken(key, token);
		}
		if (!token.has_value() || *token == 0U) {
			return PropertyValue();
		}
		const auto value = pages_.readEdgeProperty(snapshot, edgeId, *token, &active);
		return value.value_or(PropertyValue());
	}

	bool PageBackedLogicalGraphReadAdapter::edgePropertyExists(const MetadataSnapshotPin &snapshot,
																			 const uint64_t edgeId, const std::string_view key,
																			 NativeReadSession *session, const bool requireNonNull) const {
		if (key.empty()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									"page-backed Edge Property presence read requires a non-empty key");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const std::optional<std::optional<uint32_t>> cached = active.propertyToken(key);
		const std::optional<uint32_t> token = cached.has_value() ? *cached : names_.resolvePropertyToken(snapshot, key);
		if (!cached.has_value()) {
			active.putPropertyToken(key, token);
		}
		return token.has_value() && *token != 0U && pages_.edgePropertyExists(snapshot, edgeId, *token, &active, requireNonNull);
	}

	std::vector<PropertyValue> PageBackedLogicalGraphReadAdapter::readEdgePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds, const std::string_view key,
			NativeReadSession *session) const {
		if (key.empty()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									  "page-backed sorted Edge Property read requires a non-empty key");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const std::optional<std::optional<uint32_t>> cached = active.propertyToken(key);
		const std::optional<uint32_t> token = cached.has_value() ? *cached : names_.resolvePropertyToken(snapshot, key);
		if (!cached.has_value()) {
			active.putPropertyToken(key, token);
		}

		std::vector<PropertyValue> result(edgeIds.size());
		if (!token.has_value() || *token == 0U) {
			return result;
		}
		const std::vector<std::optional<PropertyValue>> values =
				pages_.readEdgePropertiesSorted(snapshot, edgeIds, *token, &active);
		if (values.size() != result.size()) { // ZYX_COV_EXCL_LINE: the native adapter always preserves input arity.
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed sorted Edge Property read changed result cardinality");
		}
		for (size_t index = 0U; index < values.size(); ++index) {
			result[index] = values[index].value_or(PropertyValue());
		}
		return result;
	}

	std::vector<std::vector<PropertyValue>> PageBackedLogicalGraphReadAdapter::readEdgePropertyColumnsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			const std::span<const std::string_view> keys, NativeReadSession *session,
			const SelectedPropertyReadAdmission *admission) const {
		validateNamedPropertyColumnRequest(edgeIds, keys);
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		auto result = makeEmptyPropertyColumns(keys.size(), edgeIds.size());
		std::vector<std::pair<uint32_t, size_t>> resolved;
		resolved.reserve(keys.size());
		for (size_t index = 0U; index < keys.size(); ++index) {
			const std::optional<std::optional<uint32_t>> cached = active.propertyToken(keys[index]);
			const std::optional<uint32_t> token =
					cached.has_value() ? *cached : names_.resolvePropertyToken(snapshot, keys[index]);
			if (!cached.has_value()) {
				active.putPropertyToken(keys[index], token);
			}
			if (token.has_value() && *token != 0U) {
				resolved.emplace_back(*token, index);
			}
		}
		std::ranges::sort(resolved, {}, &std::pair<uint32_t, size_t>::first);
		std::vector<uint32_t> tokens;
		size_t uniqueTokens = 0U;
		uint32_t previousToken = 0U;
		for (const auto &[token, outputIndex]: resolved) {
			if (token != previousToken) { ++uniqueTokens; previousToken = token; }
		}
		tokens.reserve(uniqueTokens);
		for (const auto &[token, outputIndex]: resolved)
			if (tokens.empty() || tokens.back() != token) tokens.push_back(token);
		if (tokens.empty()) {
			return result;
		}
		std::vector<std::vector<std::optional<PropertyValue>>> values =
				pages_.readEdgePropertyColumnsSorted(snapshot, edgeIds, tokens, &active, admission);
		if (values.size() != tokens.size()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed Edge Property column read changed column cardinality");
		}
		const bool traceValues = debug::PerfTrace::areValuesEnabled();
		size_t copiedValues = 0U;
		size_t copiedPayloadBytes = 0U;
		size_t resolvedIndex = 0U;
		for (size_t tokenIndex = 0U; tokenIndex < values.size(); ++tokenIndex) {
			if (values[tokenIndex].size() != edgeIds.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed Edge Property column read changed row cardinality");
			}
			size_t groupEnd = resolvedIndex;
			while (groupEnd < resolved.size() && resolved[groupEnd].first == tokens[tokenIndex]) ++groupEnd;
			for (; resolvedIndex < groupEnd; ++resolvedIndex) {
				const size_t outputIndex = resolved[resolvedIndex].second;
				const bool lastAlias = resolvedIndex + 1U == groupEnd;
				for (size_t row = 0U; row < edgeIds.size(); ++row) {
					std::optional<PropertyValue> &value = values[tokenIndex][row];
					if (traceValues && value.has_value()) {
						++copiedValues;
						const size_t valueBytes = property_utils::getPropertyValueSize(*value);
						copiedPayloadBytes = valueBytes > std::numeric_limits<size_t>::max() - copiedPayloadBytes
									? std::numeric_limits<size_t>::max()
									: copiedPayloadBytes + valueBytes;
					}
					if (admission != nullptr && value.has_value() && !lastAlias) {
						const size_t encodedBytes = CanonicalPropertyValueCodec::encodedSize(*value);
						if (encodedBytes > std::numeric_limits<size_t>::max() - sizeof(uint32_t)) {
							throw std::overflow_error("logical Property entry size overflowed");
						}
						(*admission)(encodedBytes + sizeof(uint32_t));
					}
					result[outputIndex][row] = !value.has_value() ? PropertyValue()
							: lastAlias ? std::move(*value) : *value;
				}
			}
		}
		if (traceValues) {
			debug::PerfTrace::addValue("native_graph.read.edge_selected_column_output_values",
					copiedValues > static_cast<size_t>(std::numeric_limits<int64_t>::max())
							? std::numeric_limits<int64_t>::max()
							: static_cast<int64_t>(copiedValues));
			debug::PerfTrace::addValue("native_graph.read.edge_selected_column_output_payload_bytes",
					copiedPayloadBytes > static_cast<size_t>(std::numeric_limits<int64_t>::max())
							? std::numeric_limits<int64_t>::max()
							: static_cast<int64_t>(copiedPayloadBytes));
		}
		return result;
	}

	bool PageBackedLogicalGraphReadAdapter::nodeHasLabels(const MetadataSnapshotPin &snapshot, const uint64_t nodeId,
													 const std::vector<std::string> &labels,
													 NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const std::optional<std::vector<uint64_t>> tokens = resolveNodeLabelTokens(snapshot, labels, active);
		return tokens.has_value() && pages_.nodeHasLabelTokens(snapshot, nodeId, *tokens, &active);
	}

	std::vector<bool> PageBackedLogicalGraphReadAdapter::readNodeLabelMatchesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const std::vector<std::string> &labels, NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const std::optional<std::vector<uint64_t>> tokens = resolveNodeLabelTokens(snapshot, labels, active);
		if (!tokens.has_value()) {
			return std::vector<bool>(nodeIds.size(), false);
		}
		return pages_.readNodeLabelMatchesSorted(snapshot, nodeIds, *tokens, &active);
	}

	EntityRef PageBackedLogicalGraphReadAdapter::readNodeRef(const MetadataSnapshotPin &snapshot, const uint64_t nodeId,
																																												const EntityReadShape &shape,
																																												NativeReadSession *session) const {
		// A reference always authenticates the Entity Map entry, but deliberately
		// avoids complete object materialization. Selected fields requested by the
		// shape are prefetched into the same session so a later consumer can reuse
		// the authenticated column values without another directory lookup.
		validateEntityReadShape(shape);
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		static_cast<void>(pages_.readNodeRecord(snapshot, nodeId, &active));
		if (shape.include_labels) {
			for (const uint64_t token: pages_.readNodeLabelTokens(snapshot, nodeId, &active)) {
				static_cast<void>(resolveNodeLabelName(snapshot, token, active));
			}
		}
		if (!shape.property_tokens.empty()) {
			const std::array<uint64_t, 1U> ids{nodeId};
			const std::vector<std::vector<std::optional<PropertyValue>>> columns =
					pages_.readNodePropertyColumnsSorted(snapshot, ids, shape.property_tokens, &active);
			if (columns.size() != shape.property_tokens.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed Node reference read changed Property column cardinality");
			}
			for (size_t index = 0U; index < columns.size(); ++index) {
				if (columns[index].size() != 1U) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed Node reference read changed Property row cardinality");
				}
			}
		}
		return {nodeId, false, 0U, 0U, 0U};
	}

	EntityRef PageBackedLogicalGraphReadAdapter::readEdgeRef(const MetadataSnapshotPin &snapshot, const uint64_t edgeId,
																																				const EntityReadShape &shape,
																																				NativeReadSession *session) const {
		validateEntityReadShape(shape);
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const EdgeEntityRecord edge = pages_.readEdgeTopology(snapshot, edgeId, &active);
		if (edge.source_node_id == 0U || edge.target_node_id == 0U) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed graph Edge reference has invalid endpoints");
		}
		if (shape.include_type) {
			static_cast<void>(resolveRelationshipTypeName(snapshot, edge.type_id, active));
		}
		if (!shape.property_tokens.empty()) {
			const std::array<uint64_t, 1U> ids{edgeId};
			const std::vector<std::vector<std::optional<PropertyValue>>> columns =
					pages_.readEdgePropertyColumnsSorted(snapshot, ids, shape.property_tokens, &active);
			if (columns.size() != shape.property_tokens.size()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed Edge reference read changed Property column cardinality");
			}
			for (size_t index = 0U; index < columns.size(); ++index) {
				if (columns[index].size() != 1U) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed Edge reference read changed Property row cardinality");
				}
			}
		}
		return {edgeId, true, edge.source_node_id, edge.target_node_id, edge.type_id};
	}

	bool PageBackedLogicalGraphReadAdapter::visitNodesUntil(
			const MetadataSnapshotPin &snapshot, size_t maximumRecords,
			const std::function<bool(const LogicalGraphNode &)> &visitor, NativeReadSession *session) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									 "page-backed logical graph Node scan requires a visitor");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		return pages_.visitNodesUntil(snapshot, maximumRecords,
															  [this, &snapshot, &visitor, &active](const PageBackedGraphNode &node) {
																  return visitor(materializeNode(snapshot, node, active));
															  }, &active);
	}

	bool PageBackedLogicalGraphReadAdapter::visitNodeRefsUntil(
			const MetadataSnapshotPin &snapshot, const size_t maximumRecords,
			const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
								 "page-backed logical graph Node reference scan requires a visitor");
		}
		return pages_.visitNodeRefsUntil(snapshot, maximumRecords, visitor, session);
	}

	bool PageBackedLogicalGraphReadAdapter::visitNodeRefsWithLabelMatchesUntil(
			const MetadataSnapshotPin &snapshot, const size_t maximumRecords, const std::vector<std::string> &labels,
			const std::function<bool(const EntityRef &, bool)> &visitor, NativeReadSession *session) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed logical graph labeled Node reference scan requires a visitor");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const std::optional<std::vector<uint64_t>> tokens = resolveNodeLabelTokens(snapshot, labels, active);
		if (!tokens.has_value()) {
			return pages_.visitNodeRefsUntil(snapshot, maximumRecords,
					[&visitor](const EntityRef &reference) { return visitor(reference, false); }, &active);
		}
		return pages_.visitNodeRefsWithLabelTokenMatchesUntil(snapshot, maximumRecords, *tokens, visitor, &active);
	}

	bool PageBackedLogicalGraphReadAdapter::visitEdgesUntil(
			const MetadataSnapshotPin &snapshot, size_t maximumRecords,
			const std::function<bool(const LogicalGraphEdge &)> &visitor, NativeReadSession *session) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									 "page-backed logical graph Edge scan requires a visitor");
		}
		debug::ScopedPerfTimer timer("native_graph.read.edge_logical_materialization");
		const bool tracing = debug::PerfTrace::isEnabled();
		uint64_t nameMaterializationNanoseconds = 0U;
		uint64_t visitorNanoseconds = 0U;
		uint64_t records = 0U;
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const bool exhausted = pages_.visitEdgesUntil(
				snapshot, maximumRecords,
				[this, &snapshot, &visitor, tracing, &nameMaterializationNanoseconds, &visitorNanoseconds,
				 &records, &active](const PageBackedGraphEdge &edge) {
					const auto materializationStart = tracing ? std::chrono::steady_clock::now()
																	 : std::chrono::steady_clock::time_point{};
					LogicalGraphEdge logical = materializeEdge(snapshot, edge, active);
					if (tracing) {
						nameMaterializationNanoseconds += elapsedNanoseconds(materializationStart);
					}
					const auto visitorStart = tracing ? std::chrono::steady_clock::now()
																	 : std::chrono::steady_clock::time_point{};
					const bool keepGoing = visitor(logical);
					if (tracing) {
						visitorNanoseconds += elapsedNanoseconds(visitorStart);
						++records;
					}
					return keepGoing;
				}, &active);
		if (tracing) {
			debug::PerfTrace::addDurationBatch("native_graph.read.edge_name_materialization",
											 nameMaterializationNanoseconds, records);
			debug::PerfTrace::addDurationBatch("native_graph.read.edge_query_visitor", visitorNanoseconds, records);
		}
		return exhausted;
	}

	bool PageBackedLogicalGraphReadAdapter::visitEdgeRefsUntil(
			const MetadataSnapshotPin &snapshot, const size_t maximumRecords,
			const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
								 "page-backed logical graph Edge reference scan requires a visitor");
		}
		return pages_.visitEdgeRefsUntil(snapshot, maximumRecords, visitor, session);
	}

	bool PageBackedLogicalGraphReadAdapter::visitEdgeTopologiesUntil(
			const MetadataSnapshotPin &snapshot, size_t maximumRecords,
			const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor, NativeReadSession *session) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
								 "page-backed logical graph Edge topology scan requires a visitor");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		return pages_.visitEdgeTopologiesUntil(
				snapshot, maximumRecords,
				[this, &snapshot, &visitor, &active](const uint64_t edgeId, const EdgeEntityRecord &record) {
					// Entity Map and EdgeEntityRecordCodec authenticate topology identities.
					if (edgeId == 0U || record.source_node_id == 0U || record.target_node_id == 0U) {
						throwLogicalAdapterError(
								FormatError::FERR_INVALID_STATE,
								"page-backed logical graph adapter received an Edge topology with invalid identity or endpoints");
					}
					std::string type;
					if (record.type_id != 0U) {
						type = resolveRelationshipTypeName(snapshot, record.type_id, active);
						if (type.empty()) {
							throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											 "page-backed logical graph adapter resolved an empty Edge type name");
						}
					}
					return visitor({edgeId, record.source_node_id, record.target_node_id, std::move(type)});
				}, &active);
	}

	LogicalGraphNodePage PageBackedLogicalGraphReadAdapter::scanNodesAfter(const MetadataSnapshotPin &snapshot,
																																								   uint64_t afterNodeId, size_t maximumRecords,
																																								   size_t maximumMetadataPages, NativeReadSession *session) const {
		return scanNodesAfter(snapshot, afterNodeId, maximumRecords, maximumMetadataPages,
																																								   EntityReadShape{.include_labels = true, .all_properties = true}, session);
	}

	LogicalGraphNodePage PageBackedLogicalGraphReadAdapter::scanNodesAfter(const MetadataSnapshotPin &snapshot,
																																								   uint64_t afterNodeId, size_t maximumRecords,
																																								   size_t maximumMetadataPages, const EntityReadShape &shape,
																																								   NativeReadSession *session) const {
		validateEntityReadShape(shape);
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		const PageBackedGraphNodePage raw =
				pages_.scanNodesAfter(snapshot, afterNodeId, maximumRecords, maximumMetadataPages, shape, &active);
		LogicalGraphNodePage result;
		result.may_have_more = raw.may_have_more;
		result.metadata_pages_visited = raw.metadata_pages_visited;
		result.nodes.reserve(raw.nodes.size());
		for (const PageBackedGraphNode &node: raw.nodes) {
			result.nodes.push_back(materializeNode(snapshot, node, active));
		}
		return result;
	}

	LogicalGraphAdjacencyPage PageBackedLogicalGraphReadAdapter::scanAdjacentEdges(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			NativeReadSession *session, const SelectedPropertyReadAdmission *admission,
			const std::function<void(size_t)> &retainedBufferAdmission) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		PageBackedGraphAdjacencyPage raw = pages_.scanAdjacentEdges(
				snapshot, nodeId, pageDirection(direction), pageContinuation(continuation), maximumRecords, &active, admission, retainedBufferAdmission);
		const auto checkedBytes = [](size_t base, size_t count, size_t width) {
			if (width != 0U && count > (std::numeric_limits<size_t>::max() - base) / width) {
				throwLogicalAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "logical adjacency buffer size overflowed");
			}
			const size_t bytes = base + count * width;
			return bytes;
		};
		const auto admitBuffers = [&](size_t base, size_t count, size_t width) {
			const size_t bytes = checkedBytes(base, count, width);
			if (retainedBufferAdmission) retainedBufferAdmission(bytes);
			return bytes;
		};
		size_t rawBytes = checkedBytes(0U, raw.edges.capacity(), sizeof(PageBackedGraphEdge));
		for (const auto &edge : raw.edges) {
			rawBytes = checkedBytes(rawBytes, edge.properties.capacity(), sizeof(PropertyCollectionRootEntry));
		}
		admitBuffers(rawBytes, 0U, 0U);
		admitBuffers(rawBytes, raw.edges.size(), sizeof(LogicalGraphEdge));
		LogicalGraphAdjacencyPage result;
		result.edges.reserve(raw.edges.size());
		const size_t conversionBytes = admitBuffers(rawBytes, result.edges.capacity(), sizeof(LogicalGraphEdge));
		size_t typeBytes = 0U, propertySlotsBytes = 0U;
		for (PageBackedGraphEdge &edge: raw.edges) {
			size_t currentPropertySlots = 0U, currentTypeBytes = 0U, temporaryBytes = 0U;
			const auto baseBytes = [&] { return checkedBytes(checkedBytes(conversionBytes, propertySlotsBytes, 1U), typeBytes, 1U); };
			auto logical = materializeEdgeImpl(snapshot, edge, active,
				[&](size_t bytes) { currentTypeBytes = bytes; admitBuffers(checkedBytes(checkedBytes(baseBytes(), temporaryBytes, 1U), currentPropertySlots, 1U), bytes, 1U); },
				[&](size_t bytes) { currentPropertySlots = bytes; admitBuffers(checkedBytes(checkedBytes(baseBytes(), temporaryBytes, 1U), currentTypeBytes, 1U), bytes, 1U); },
				[&](size_t bytes) { temporaryBytes = bytes; admitBuffers(checkedBytes(checkedBytes(baseBytes(), currentPropertySlots, 1U), currentTypeBytes, 1U), bytes, 1U); });
			propertySlotsBytes = checkedBytes(propertySlotsBytes, currentPropertySlots, 1U);
			typeBytes = checkedBytes(typeBytes, currentTypeBytes, 1U);
			admitBuffers(checkedBytes(conversionBytes, propertySlotsBytes, 1U), typeBytes, 1U);
			result.edges.push_back(std::move(logical));
		}
		result.continuation = logicalContinuation(raw.continuation);
		admitBuffers(checkedBytes(typeBytes, propertySlotsBytes, 1U), result.edges.capacity(), sizeof(LogicalGraphEdge));
		return result;
	}

	LogicalGraphAdjacencyTopologyPage PageBackedLogicalGraphReadAdapter::scanAdjacentEdgeTopologies(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, const LogicalGraphReadDirection direction,
			const std::optional<LogicalGraphAdjacencyContinuation> continuation, const size_t maximumRecords,
			NativeReadSession *session, const std::function<void(size_t)> &retainedBufferAdmission) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		PageBackedGraphAdjacencyTopologyPage raw = pages_.scanAdjacentEdgeTopologies(
				snapshot, nodeId, pageDirection(direction), pageContinuation(continuation), maximumRecords, &active, retainedBufferAdmission);
		if (raw.edges.size() != raw.edge_ids.size()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph topology scan returned mismatched Edge identities and records");
		}
		const auto checkedBytes = [](size_t base, size_t count, size_t width) {
			if (width != 0U && count > (std::numeric_limits<size_t>::max() - base) / width) {
				throwLogicalAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "logical topology buffer size overflowed");
			}
			return base + count * width;
		};
		const size_t rawBytes = checkedBytes(checkedBytes(0U, raw.edge_ids.capacity(), sizeof(uint64_t)),
			raw.edges.capacity(), sizeof(EdgeEntityRecord));
		const auto admit = [&](size_t bytes) { if (retainedBufferAdmission) retainedBufferAdmission(bytes); };
		LogicalGraphAdjacencyTopologyPage result;
		admit(checkedBytes(rawBytes, raw.edges.size(), sizeof(LogicalGraphEdgeTopology)));
		result.edges.reserve(raw.edges.size());
		const size_t conversionBytes = checkedBytes(rawBytes, result.edges.capacity(), sizeof(LogicalGraphEdgeTopology));
		admit(conversionBytes);
		size_t typeBytes = 0U;
		for (size_t index = 0U; index < raw.edges.size(); ++index) {
			const EdgeEntityRecord &edge = raw.edges[index];
			const uint64_t edgeId = raw.edge_ids[index];
			if (edgeId == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											  "page-backed graph adjacency topology has invalid identity or endpoints");
			}
			std::string type;
			if (edge.type_id != 0U) {
				type = resolveRelationshipTypeName(snapshot, edge.type_id, active,
					[&](size_t bytes) { admit(checkedBytes(checkedBytes(conversionBytes, typeBytes, 1U), bytes, 1U)); });
				if (type.empty()) {
					throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
											  "page-backed graph adjacency topology resolved an empty Edge type");
				}
			}
			typeBytes = checkedBytes(typeBytes, type.capacity(), 1U);
			admit(checkedBytes(conversionBytes, typeBytes, 1U));
			result.edges.push_back({edgeId, edge.source_node_id, edge.target_node_id, std::move(type)});
		}
		result.continuation = logicalContinuation(raw.continuation);
		std::vector<uint64_t>().swap(raw.edge_ids);
		std::vector<EdgeEntityRecord>().swap(raw.edges);
		admit(checkedBytes(checkedBytes(0U, result.edges.capacity(), sizeof(LogicalGraphEdgeTopology)), typeBytes, 1U));
		return result;
	}

	LogicalGraphAdjacencyReferencePage PageBackedLogicalGraphReadAdapter::scanAdjacentEdgeRefs(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, const LogicalGraphReadDirection direction,
			const std::optional<LogicalGraphAdjacencyContinuation> continuation, const size_t maximumRecords,
			NativeReadSession *session, const std::function<void(size_t)> &retainedBufferAdmission) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		PageBackedGraphAdjacencyTopologyPage raw = pages_.scanAdjacentEdgeTopologies(
				snapshot, nodeId, pageDirection(direction), pageContinuation(continuation), maximumRecords, &active, retainedBufferAdmission);
		if (raw.edges.size() != raw.edge_ids.size()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph reference scan returned mismatched Edge identities and records");
		}
		const auto checkedBytes = [](size_t base, size_t count, size_t width) {
			if (count > (std::numeric_limits<size_t>::max() - base) / width)
				throwLogicalAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "logical reference buffer size overflowed");
			return base + count * width;
		};
		const auto admit = [&](size_t bytes) { if (retainedBufferAdmission) retainedBufferAdmission(bytes); };
		const size_t rawBytes = checkedBytes(checkedBytes(0U, raw.edge_ids.capacity(), sizeof(uint64_t)),
			raw.edges.capacity(), sizeof(EdgeEntityRecord));
		LogicalGraphAdjacencyReferencePage result;
		admit(checkedBytes(rawBytes, raw.edges.size(), sizeof(EntityRef)));
		result.edges.reserve(raw.edges.size());
		admit(checkedBytes(rawBytes, result.edges.capacity(), sizeof(EntityRef)));
		for (size_t index = 0U; index < raw.edges.size(); ++index) {
			const EdgeEntityRecord &edge = raw.edges[index];
			const uint64_t edgeId = raw.edge_ids[index];
			// type_id == 0 is the durable representation of an untyped
			// relationship.  A topology reference must authenticate identity and
			// endpoints, but it must not reject the valid untyped case.
			if (edgeId == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph adjacency reference has invalid identity, endpoints, or type token");
			}
			result.edges.push_back({edgeId, true, edge.source_node_id, edge.target_node_id, edge.type_id});
		}
		result.continuation = logicalContinuation(raw.continuation);
		std::vector<uint64_t>().swap(raw.edge_ids);
		std::vector<EdgeEntityRecord>().swap(raw.edges);
		admit(checkedBytes(0U, result.edges.capacity(), sizeof(EntityRef)));
		return result;
	}

	bool PageBackedLogicalGraphReadAdapter::visitAdjacentEdgeRefsForNodesUntil(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const LogicalGraphReadDirection direction, const size_t maximumRecords,
			const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session,
			const std::function<void(size_t)> &retainedBufferAdmission) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		return pages_.visitAdjacentEdgeRefsForNodesUntil(snapshot, nodeIds, pageDirection(direction), maximumRecords,
				[&visitor](const EntityRef &reference) { return visitor(reference); }, &active, retainedBufferAdmission);
	}

	PageBackedLogicalGraphAdjacencyCursor PageBackedLogicalGraphReadAdapter::openAdjacentEdgeCursor(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			NativeReadSession *session) const {
		std::unique_ptr<NativeReadSession> ownedSession;
		NativeReadSession *active = session;
		if (active == nullptr) {
			const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
                (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr);
			ownedSession.reset(new (memory) NativeReadSession(snapshot));
			active = ownedSession.get();
		}
		active->bind(snapshot);
		return PageBackedLogicalGraphAdjacencyCursor(*this, pages_.openAdjacentEdgeCursor(
				snapshot, nodeId, pageDirection(direction), active), active, std::move(ownedSession));
	}

	bool PageBackedLogicalGraphReadAdapter::visitAdjacentEdgesUntil(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor,
			NativeReadSession *session) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									 "page-backed logical graph adjacency traversal requires a visitor");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		return pages_.visitAdjacentEdgesUntil(snapshot, nodeId, pageDirection(direction), maximumRecords,
												  [this, &snapshot, &visitor, &active](const PageBackedGraphEdge &edge) {
													  return visitor(materializeEdge(snapshot, edge, active));
												  }, &active);
	}

	bool PageBackedLogicalGraphReadAdapter::visitAdjacentEdgeTopologiesUntil(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			size_t maximumRecords, const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor,
			NativeReadSession *session) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									 "page-backed logical graph adjacency topology traversal requires a visitor");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = activeSession(snapshot, session, localSession);
		return pages_.visitAdjacentEdgeTopologiesUntil(
				snapshot, nodeId, pageDirection(direction), maximumRecords,
				[this, &snapshot, &visitor, &active](uint64_t edgeId, const EdgeEntityRecord &edge) {
					// Entity Map and EdgeEntityRecordCodec authenticate adjacency identities.
					if (edgeId == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
						throwLogicalAdapterError(
								FormatError::FERR_INVALID_STATE,
								"page-backed logical graph adapter received invalid adjacency topology identity");
					}
					std::string type;
					if (edge.type_id != 0U) {
						type = resolveRelationshipTypeName(snapshot, edge.type_id, active);
						if (type.empty()) {
							throwLogicalAdapterError(
									FormatError::FERR_INVALID_STATE,
									"page-backed logical graph adapter resolved an empty adjacency Edge type");
						}
					}
					return visitor({edgeId, edge.source_node_id, edge.target_node_id, std::move(type)});
				}, &active);
	}

} // namespace graph::storage
