/**
 * @file PageBackedGraphReadAdapter.cpp
 * @brief Logical graph records reconstructed from native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>
#include <type_traits>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

namespace graph::storage {

	namespace {

		// This is a runtime work bound rather than a B+Tree layout capacity. It
		// keeps visitor traversal responsive to early termination even for a
		// supernode that spans many packed leaves.
		constexpr size_t ADJACENCY_VISIT_BATCH_ENTRIES = 256U;
		// Multi-source scans amortize Entity Map routing over a larger but still
		// fixed batch. At current record widths this remains well below one MiB.
		constexpr size_t MULTI_NODE_ADJACENCY_BATCH_ENTRIES = 4'096U;

		[[noreturn]] void throwAdapterError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] GlobalAdjacencyDirection globalDirection(PageBackedGraphReadDirection direction) {
			if (direction == PageBackedGraphReadDirection::PBGRD_OUTGOING) {
				return GlobalAdjacencyDirection::GAD_OUT;
			}
			if (direction == PageBackedGraphReadDirection::PBGRD_INCOMING) {
				return GlobalAdjacencyDirection::GAD_IN;
			}
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency scan has an unknown direction");
		}

		[[nodiscard]] uint64_t elapsedNanoseconds(const std::chrono::steady_clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
		}

	} // namespace

	PageBackedGraphAdjacencyCursor::~PageBackedGraphAdjacencyCursor() = default;
	PageBackedGraphAdjacencyCursor::PageBackedGraphAdjacencyCursor(PageBackedGraphAdjacencyCursor &&) noexcept =
			default;
	PageBackedGraphAdjacencyCursor &
	PageBackedGraphAdjacencyCursor::operator=(PageBackedGraphAdjacencyCursor &&) noexcept = default;

	PageBackedGraphAdjacencyPage
	PageBackedGraphAdjacencyCursor::next(const MetadataSnapshotPin &snapshot,
										 std::optional<PageBackedGraphAdjacencyContinuation> continuation,
										 size_t maximumRecords) {
		if (adapter_ == nullptr || property_cursor_ == nullptr) {
			throwAdapterError(FormatError::FERR_INVALID_STATE, "page-backed adjacency cursor has no scan state");
		}
		return adapter_->scanAdjacentEdges(snapshot, node_id_, direction_, continuation, maximumRecords,
												 property_cursor_.get(), session_);
	}

	PageBackedGraphReadAdapter::PageBackedGraphReadAdapter(RelocatableMetadataStore &metadata,
																   EntityPageStore &entities, EntityLabelSetStore &labels,
																   PropertyCollectionRootStore &properties,
																   BTreePageStore &indexes,
																   size_t maximumAdjacencyEdges) :
		metadata_(metadata), entities_(entities), labels_(labels), properties_(properties),
		adjacency_(indexes, metadata), maximum_adjacency_edges_(maximumAdjacencyEdges) {
		if (!entities_.usesMetadataStore(metadata_) || !labels_.usesMetadataStore(metadata_) ||
			!properties_.usesMetadataStore(metadata_) || !indexes.usesMetadataStore(metadata_) ||
			maximum_adjacency_edges_ == 0U ||
			maximum_adjacency_edges_ == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument("page-backed graph read adapter requires stores in one metadata domain");
		}
	}

	std::vector<PropertyCollectionRootEntry>
	PageBackedGraphReadAdapter::readProperties(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
											   PropertyCollectionRootScanCursor *scanCursor, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) const {
		std::optional<PropertyCollectionRoot> root = scanCursor == nullptr
															 ? properties_.readEntityOptional(snapshot, key, admission, slotsAdmission)
															 : scanCursor->readEntityOptional(snapshot, key, admission, slotsAdmission);
		if (!root.has_value()) {
			return {};
		}
		return std::move(root->entries);
	}

	PageBackedGraphNode
	PageBackedGraphReadAdapter::materializeNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
												const NodeEntityRecord &record,
												PropertyCollectionRootScanCursor *scanCursor) const {
		return {nodeId, labels_.readNodeLabels(snapshot, record),
				readProperties(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}, scanCursor)};
	}

	PageBackedGraphEdge
	PageBackedGraphReadAdapter::materializeEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
												const EdgeEntityRecord &record,
												PropertyCollectionRootScanCursor *scanCursor, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) const {
		return {edgeId, record.source_node_id, record.target_node_id, record.type_id,
				readProperties(snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId}, scanCursor, admission, slotsAdmission)};
	}

	PageBackedGraphNode PageBackedGraphReadAdapter::readNode(const MetadataSnapshotPin &snapshot,
															 uint64_t nodeId, NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (nodeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Node read requires a nonzero logical Node ID");
		}
		// EntityPageStore resolves NodeId through the snapshot-pinned Entity Map.
		const std::optional<NodeEntityRecord> cached = activeSession.node(nodeId);
		debug::PerfTrace::addValue(cached.has_value() ? "native_graph.read.entity_node_record_cache_hit"
															 : "native_graph.read.entity_node_record_cache_miss",
													 1);
		const NodeEntityRecord record = cached.has_value() ? *cached : entities_.readNode(snapshot, nodeId);
		if (!cached.has_value()) {
			activeSession.putNode(nodeId, record);
		}
		// Reuse the session-owned Property cursor for complete reads.  Without
		// this hand-off each materialized Node starts a fresh directory walk,
		// defeating the snapshot-bound scan locality promised by NativeReadSession.
		return materializeNode(snapshot, nodeId, record, &activeSession.propertyCursor(properties_, snapshot));
	}

	NodeEntityRecord PageBackedGraphReadAdapter::readNodeRecord(const MetadataSnapshotPin &snapshot,
																							 const uint64_t nodeId, NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (nodeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Node record read requires a nonzero logical Node ID");
		}
		const std::optional<NodeEntityRecord> cached = activeSession.node(nodeId);
		debug::PerfTrace::addValue(cached.has_value() ? "native_graph.read.entity_node_record_cache_hit"
															 : "native_graph.read.entity_node_record_cache_miss",
													 1);
		const NodeEntityRecord record = cached.has_value() ? *cached : entities_.readNode(snapshot, nodeId);
		if (!cached.has_value()) {
			activeSession.putNode(nodeId, record);
		}
		return record;
	}

	std::vector<NodeEntityRecord> PageBackedGraphReadAdapter::readNodeRecordsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (nodeIds.empty()) {
			return {};
		}
		uint64_t previousNodeId = 0U;
		std::vector<uint64_t> missingIds;
		missingIds.reserve(nodeIds.size());
		for (const uint64_t nodeId: nodeIds) {
			if (nodeId == 0U || nodeId <= previousNodeId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed shaped Node batch requires strictly increasing Node IDs");
			}
			previousNodeId = nodeId;
			if (!activeSession.node(nodeId).has_value()) {
				missingIds.push_back(nodeId);
			}
		}
		const std::vector<std::optional<NodeEntityRecord>> missing =
				missingIds.empty() ? std::vector<std::optional<NodeEntityRecord>>{}
											: entities_.readNodesSorted(snapshot, missingIds);
		if (missing.size() != missingIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed shaped Node batch changed Entity record cardinality");
		}
		// Keep the batch result independent from the bounded session cache. A batch
		// may legitimately contain more entities than the cache can retain; using
		// the cache as the result buffer would make early IDs disappear while the
		// result is being reconstructed.
		std::vector<NodeEntityRecord> resolved(nodeIds.size());
		std::vector<bool> resolvedFlags(nodeIds.size(), false);
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			if (const std::optional<NodeEntityRecord> cached = activeSession.node(nodeIds[index]); cached.has_value()) {
				resolved[index] = *cached;
				resolvedFlags[index] = true;
			}
		}
		for (size_t index = 0U; index < missing.size(); ++index) {
			if (!missing[index].has_value()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed shaped Node batch found a missing Entity record");
			}
			const auto ordinal = std::ranges::lower_bound(nodeIds, missingIds[index]);
			if (ordinal == nodeIds.end() || *ordinal != missingIds[index]) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed shaped Node batch lost its logical ID alignment");
			}
			const size_t resultIndex = static_cast<size_t>(ordinal - nodeIds.begin());
			resolved[resultIndex] = *missing[index];
			resolvedFlags[resultIndex] = true;
			activeSession.putNode(missingIds[index], *missing[index]);
		}
		for (const bool resolvedFlag: resolvedFlags) {
			if (!resolvedFlag) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed shaped Node batch lost an authenticated Entity record");
			}
		}
		return resolved;
	}

	std::vector<uint32_t> PageBackedGraphReadAdapter::readEntityPropertyTokens(
		const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
		const SelectedPropertyReadAdmission &tokenAdmission,
		const SelectedPropertyReadAdmission &transientAdmission, NativeReadSession *session, const CancellationCheckpoint *cancellation) const {
		if (cancellation) cancellation->check();
		if (!key.isValid() || !tokenAdmission || !transientAdmission)
			throwAdapterError(FormatError::FERR_INVALID_HEADER, "property token read requires valid owner and admissions");
		std::optional<NativeReadSession> localSession;
		auto &active = selectNativeReadSession(snapshot, session, localSession);
		if (key.owner_kind == EntityPropertyOwnerKind::EPOK_NODE) {
			static_cast<void>(readNodeRecord(snapshot, key.entity_id, &active));
		} else {
			const std::array<uint64_t, 1U> ids{key.entity_id};
			static_cast<void>(readEdgeTopologiesSorted(snapshot, ids, &active));
		}
		if (cancellation) cancellation->check();
		return properties_.readEntityPropertyTokens(snapshot, key, tokenAdmission, transientAdmission, cancellation);
	}

	std::vector<PropertyCollectionRootEntry> PageBackedGraphReadAdapter::readNodeProperties(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, NativeReadSession *session, const SelectedPropertyReadAdmission *admission) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		static_cast<void>(readNodeRecord(snapshot, nodeId, &active));
		return readProperties(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId},
																										 &active.propertyCursor(properties_, snapshot), admission);
	}

	std::vector<std::vector<PropertyCollectionRootEntry>> PageBackedGraphReadAdapter::readNodePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			NativeReadSession *session, const SelectedPropertyReadAdmission *admission) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		uint64_t previousId = 0U;
		for (const uint64_t id : nodeIds) {
			if (id == 0U || id <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
					"page-backed sorted Node Property root read requires strictly increasing Node IDs");
			}
			previousId = id;
		}
		std::vector<std::vector<PropertyCollectionRootEntry>> result(nodeIds.size());
		PropertyCollectionRootScanCursor &cursor = active.propertyCursor(properties_, snapshot);
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			const uint64_t nodeId = nodeIds[index];
			if (auto root = cursor.readEntityOptional(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}, admission);
				root.has_value()) {
				result[index] = std::move(root->entries);
			}
		}
		return result;
	}

	std::vector<uint64_t> PageBackedGraphReadAdapter::readNodeLabelTokens(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (nodeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Node label read requires a nonzero logical Node ID");
		}
		if (const auto cached = activeSession.nodeLabels(nodeId); cached.has_value()) {
			return *cached;
		}
		const NodeEntityRecord record = readNodeRecord(snapshot, nodeId, &activeSession);
		const std::vector<uint64_t> labels = labels_.readNodeLabels(snapshot, record);
		activeSession.putNodeLabels(nodeId, labels);
		return labels;
	}

	std::vector<std::vector<uint64_t>> PageBackedGraphReadAdapter::readNodeLabelTokensSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (nodeIds.empty()) {
			return {};
		}
		if (std::ranges::any_of(nodeIds, [](const uint64_t id) { return id == 0U; }) ||
			!std::ranges::is_sorted(nodeIds) || std::ranges::adjacent_find(nodeIds) != nodeIds.end()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
					"page-backed sorted Node label batch requires strictly increasing nonzero Node IDs");
		}
		const std::vector<NodeEntityRecord> records = readNodeRecordsSorted(snapshot, nodeIds, &activeSession);
		if (records.size() != nodeIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed sorted Node label batch changed record cardinality");
		}
		const std::vector<std::vector<uint64_t>> labels = labels_.readNodeLabelsSorted(snapshot, records);
		if (labels.size() != nodeIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed sorted Node label batch changed label cardinality");
		}
		for (size_t index = 0U; index < labels.size(); ++index) {
			activeSession.putNodeLabels(nodeIds[index], labels[index]);
		}
		return labels;
	}

	std::optional<PropertyValue> PageBackedGraphReadAdapter::readNodeProperty(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, const uint32_t propertyToken,
			NativeReadSession *session) const {
		if (nodeId == 0U || propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed Node Property read requires nonzero identifiers");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (const auto cached = activeSession.property(false, nodeId, propertyToken); cached.has_value()) {
			return *cached;
		}
		// A prior presence probe is an authenticated negative result for this
		// snapshot. Avoid descending the Property Directory again just to decode
		// a value that cannot exist.
		if (const auto present = activeSession.propertyPresence(false, nodeId, propertyToken);
			present.has_value() && !*present) {
			activeSession.putProperty(false, nodeId, propertyToken, std::nullopt);
			return std::nullopt;
		}
		const auto entry = activeSession.propertyCursor(properties_, snapshot).readEntityPropertyOptional(
				snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}, propertyToken);
		const std::optional<PropertyValue> value = entry.has_value() ? std::optional<PropertyValue>{entry->value} : std::nullopt;
		activeSession.putProperty(false, nodeId, propertyToken, value);
		return value;
	}

	bool PageBackedGraphReadAdapter::nodePropertyExists(const MetadataSnapshotPin &snapshot, const uint64_t nodeId,
																		 const uint32_t propertyToken, NativeReadSession *session, const bool requireNonNull) const {
		if (nodeId == 0U || propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
								"page-backed Node Property presence read requires nonzero identifiers");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (const auto cachedValue = activeSession.property(false, nodeId, propertyToken); cachedValue.has_value()) {
			const bool present = cachedValue->has_value();
			if (requireNonNull) return present && !std::holds_alternative<std::monostate>((*cachedValue)->getVariant());
			activeSession.putPropertyPresence(false, nodeId, propertyToken, present);
			return present;
		}
		if (const auto cached = activeSession.propertyPresence(false, nodeId, propertyToken); cached.has_value()) {
			if (!requireNonNull || !*cached) return *cached;
		}
		const bool present = activeSession.propertyCursor(properties_, snapshot).readEntityPropertyExists(
				snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}, propertyToken, requireNonNull);
		if (!requireNonNull || present) activeSession.putPropertyPresence(false, nodeId, propertyToken, present);
		return present;
	}

	std::vector<std::optional<PropertyValue>> PageBackedGraphReadAdapter::readNodePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const uint32_t propertyToken, NativeReadSession *session) const {
		if (propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed sorted Node Property read requires a nonzero Property token");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		std::vector<std::optional<PropertyValue>> result(nodeIds.size());
		std::vector<uint64_t> missingIds;
		std::vector<size_t> missingOrdinals;
		missingIds.reserve(nodeIds.size());
		missingOrdinals.reserve(nodeIds.size());
		uint64_t previousId = 0U;
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			if (nodeIds[index] == 0U || nodeIds[index] <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed sorted Node Property read requires strictly increasing Node IDs");
			}
			previousId = nodeIds[index];
			if (const auto cached = active.property(false, nodeIds[index], propertyToken); cached.has_value()) {
				result[index] = *cached;
			} else if (const auto present = active.propertyPresence(false, nodeIds[index], propertyToken);
					present.has_value() && !*present) {
				// A presence probe is an authenticated negative result for this
				// snapshot. Do not reopen the Property Directory just to rediscover
				// the same missing value in a selected batch.
				result[index] = std::nullopt;
			} else {
				missingIds.push_back(nodeIds[index]);
				missingOrdinals.push_back(index);
			}
		}
		if (missingIds.empty()) {
			return result;
		}
		std::vector<EntityPropertyKey> keys;
		keys.reserve(nodeIds.size());
		for (const uint64_t nodeId: missingIds) {
			keys.push_back({EntityPropertyOwnerKind::EPOK_NODE, nodeId});
		}
		std::vector<std::optional<PropertyCollectionRootEntry>> entries(keys.size());
		const std::array<uint32_t, 1U> selectedTokens{propertyToken};
		active.propertyCursor(properties_, snapshot).readEntityPropertiesOptionalSorted(
				snapshot, keys, selectedTokens, entries, false);
		for (size_t index = 0U; index < entries.size(); ++index) {
			const std::optional<PropertyValue> value = entries[index].has_value()
					? std::optional<PropertyValue>{entries[index]->value} : std::nullopt;
			result[missingOrdinals[index]] = value;
			active.putProperty(false, missingIds[index], propertyToken, value);
		}
		return result;
	}

	std::vector<std::vector<std::optional<PropertyValue>>>
	PageBackedGraphReadAdapter::readNodePropertyColumnsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const std::span<const uint32_t> propertyTokens, NativeReadSession *session,
			const SelectedPropertyReadAdmission *admission) const {
		if (propertyTokens.empty()) {
			return {};
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		if (nodeIds.size() > std::numeric_limits<size_t>::max() / propertyTokens.size()) {
			throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW,
							  "page-backed Node Property column read result size overflows");
		}
		uint64_t previousId = 0U;
		uint32_t previousToken = 0U;
		for (const uint32_t token: propertyTokens) {
			if (token == 0U || token <= previousToken) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed Node Property columns require strictly increasing nonzero tokens");
			}
			previousToken = token;
		}
		for (size_t owner = 0U; owner < nodeIds.size(); ++owner) {
			if (nodeIds[owner] == 0U || nodeIds[owner] <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed Node Property columns require strictly increasing Node IDs");
			}
			previousId = nodeIds[owner];
		}
		std::vector<std::vector<std::optional<PropertyValue>>> columns;
		columns.reserve(propertyTokens.size());
		for (size_t token = 0U; token < propertyTokens.size(); ++token) columns.emplace_back(nodeIds.size());
		const auto needsDecode = [&](uint64_t id) {
			return std::ranges::any_of(propertyTokens, [&](uint32_t token) {
				if (active.hasProperty(false, id, token)) return false;
				const auto present = active.propertyPresence(false, id, token);
				return !present.has_value() || *present;
			});
		};
		const size_t missingCount = static_cast<size_t>(std::ranges::count_if(nodeIds, needsDecode));
		std::vector<uint64_t> missingIds;
		missingIds.reserve(missingCount);
		debug::PerfTrace::addValue("native_graph.read.property_column_gather_capacity", static_cast<int64_t>(missingIds.capacity()));
		for (size_t owner = 0U; owner < nodeIds.size(); ++owner) {
			// If any token needs decoding, this owner is read as one rectangular
			// token batch. Avoid copying/admitting cache cells that would then be
			// decoded again and overwritten by that same immutable owner read.
			const bool missing = needsDecode(nodeIds[owner]);
			if (missing) {
				missingIds.push_back(nodeIds[owner]);
				continue;
			}
			for (size_t tokenIndex = 0U; tokenIndex < propertyTokens.size(); ++tokenIndex) {
				const uint32_t token = propertyTokens[tokenIndex];
				if (auto cached = active.property(false, nodeIds[owner], token, admission); cached.has_value()) {
					columns[tokenIndex][owner] = std::move(*cached);
				} else if (const auto present = active.propertyPresence(false, nodeIds[owner], token);
						present.has_value() && !*present) {
					columns[tokenIndex][owner] = std::nullopt;
				}
			}
		}
		if (!missingIds.empty()) {
			std::vector<EntityPropertyKey> missingKeys;
			missingKeys.reserve(missingIds.size());
			for (const uint64_t nodeId: missingIds) {
				missingKeys.push_back({EntityPropertyOwnerKind::EPOK_NODE, nodeId});
			}
			std::vector<std::optional<PropertyCollectionRootEntry>> entries(
				missingIds.size() * propertyTokens.size());
			// Reuse the query-owned ordered cursor so the Property Directory root/leaf
			// route and the current decoded page are shared across adjacent batches.
			active.propertyCursor(properties_, snapshot).readEntityPropertiesOptionalSorted(
					snapshot, missingKeys, propertyTokens, entries, true, admission);
			size_t outputOwner = 0U;
			for (size_t owner = 0U; owner < missingIds.size(); ++owner) {
				while (nodeIds[outputOwner] < missingIds[owner]) ++outputOwner;
				for (size_t token = 0U; token < propertyTokens.size(); ++token) {
					auto &entry = entries[owner * propertyTokens.size() + token];
					std::optional<PropertyValue> value = entry.has_value()
							? std::optional<PropertyValue>{std::move(entry->value)} : std::nullopt;
					// The session keeps its own snapshot-local copy; the decoded owner
					// transfers its payload into the returned column.
					active.putProperty(false, missingIds[owner], propertyTokens[token], value);
					columns[token][outputOwner] = std::move(value);
				}
			}
		}
		return columns;
	}

	void PageBackedGraphReadAdapter::readNodePropertyCellsSorted(const MetadataSnapshotPin &snapshot,
		std::span<const uint64_t> ids, std::span<const uint32_t> tokens,
		const std::function<void(size_t, size_t, std::optional<PropertyValue>)> &sink,
		NativeReadSession *session, const SelectedPropertyReadAdmission *admission, const PropertyScalarReadSink *scalars,
        const PropertyTextReadSink *texts) const {
		if (!sink || (scalars && !*scalars) || (texts && !*texts)) throw std::invalid_argument("direct Property cells require a sink");
		if (ids.size() > std::numeric_limits<size_t>::max() / sizeof(EntityPropertyKey))
			throw std::overflow_error("direct Property owner staging overflowed");
		if (admission) (*admission)(ids.size() * sizeof(EntityPropertyKey));
		std::vector<EntityPropertyKey> keys; keys.reserve(ids.size());
		for (const auto id : ids) keys.push_back({EntityPropertyOwnerKind::EPOK_NODE, id});
		std::optional<NativeReadSession> localSession;
		auto &active = selectNativeReadSession(snapshot, session, localSession);
		const auto emitValue = [&](size_t owner, size_t column, std::optional<PropertyCollectionRootEntry> entry) {
				sink(owner, column, entry ? std::optional<PropertyValue>(std::move(entry->value)) : std::nullopt);
			};
		const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> emit = std::ref(emitValue);
		active.propertyCursor(properties_, snapshot).readEntityPropertiesOptionalSorted(
			snapshot, keys, tokens, {}, true, admission, &emit, scalars, texts);
		debug::PerfTrace::addValue("native_graph.read.direct_property_batches", 1);
	}

	std::pair<std::vector<std::vector<std::optional<PropertyValue>>>,
				std::vector<std::optional<size_t>>>
	PageBackedGraphReadAdapter::readNodePropertyColumnsAndCountsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const std::span<const uint32_t> propertyTokens, NativeReadSession *session,
			const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *transientBufferAdmission) const {
		if (propertyTokens.empty()) {
			if (nodeIds.size() > std::numeric_limits<size_t>::max() / sizeof(std::optional<size_t>))
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "empty combined Property count slots overflowed");
			if (transientBufferAdmission) (*transientBufferAdmission)(nodeIds.size() * sizeof(std::optional<size_t>));
			std::vector<std::optional<size_t>> counts(nodeIds.size(), size_t{0U});
			if (transientBufferAdmission) (*transientBufferAdmission)(counts.capacity() * sizeof(std::optional<size_t>));
			return {{}, std::move(counts)};
		}		if (nodeIds.size() > std::numeric_limits<size_t>::max() / propertyTokens.size()) {
			throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW,
							  "combined Node Property read result size overflows");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		size_t stagingBytes = 0U;
		const auto admitStaging = [&](size_t extra) {
			if (extra > std::numeric_limits<size_t>::max() - stagingBytes)
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "combined Property staging overflowed");
			if (transientBufferAdmission) (*transientBufferAdmission)(stagingBytes + extra);
		};
		const auto reserveStaging = [&](auto &values, size_t count, bool resize = false) {
			using Value = typename std::remove_reference_t<decltype(values)>::value_type;
			if (count > std::numeric_limits<size_t>::max() / sizeof(Value))
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "combined Property staging slots overflowed");
			admitStaging(count * sizeof(Value));
			values.reserve(count);
			admitStaging(values.capacity() * sizeof(Value));
			stagingBytes += values.capacity() * sizeof(Value);
			if (resize) values.resize(count);
		};
		const SelectedPropertyReadAdmission admitNested = [&](size_t bytes) { admitStaging(bytes); };

		std::vector<EntityPropertyKey> keys;
		reserveStaging(keys, nodeIds.size());
		uint64_t previousId = 0U;
		for (const uint64_t nodeId: nodeIds) {
			if (nodeId == 0U || nodeId <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "combined Node Property read requires strictly increasing Node IDs");
			}
			previousId = nodeId;
			keys.push_back({EntityPropertyOwnerKind::EPOK_NODE, nodeId});
		}
		std::vector<std::optional<size_t>> counts;
		reserveStaging(counts, nodeIds.size(), true);
		std::vector<std::vector<std::optional<PropertyValue>>> columns;
		reserveStaging(columns, propertyTokens.size(), true);
		for (auto &column : columns) reserveStaging(column, nodeIds.size(), true);
		std::vector<size_t> missingOwners;
		std::vector<size_t> countOnlyOwners;
		reserveStaging(missingOwners, nodeIds.size());
		reserveStaging(countOnlyOwners, nodeIds.size());
		for (size_t owner = 0U; owner < nodeIds.size(); ++owner) {
			const bool missingValue = std::ranges::any_of(propertyTokens, [&](uint32_t token) {
				return !active.hasProperty(false, nodeIds[owner], token);
			});
			if (!missingValue) {
				for (size_t token = 0U; token < propertyTokens.size(); ++token) {
					auto cached = active.property(false, nodeIds[owner], propertyTokens[token], admission);
					if (cached) columns[token][owner] = std::move(*cached);
				}
			}
			if (missingValue) {
				missingOwners.push_back(owner);
			} else {
				countOnlyOwners.push_back(owner);
			}
		}
		const size_t baseStagingBytes = stagingBytes;
		if (!missingOwners.empty()) {
			std::vector<EntityPropertyKey> missingKeys;
			reserveStaging(missingKeys, missingOwners.size());
			std::vector<std::optional<PropertyCollectionRootEntry>> missingEntries;
			reserveStaging(missingEntries, missingOwners.size() * propertyTokens.size(), true);
			std::vector<std::optional<size_t>> missingCounts;
			reserveStaging(missingCounts, missingOwners.size(), true);
			for (const size_t owner: missingOwners) {
				missingKeys.push_back(keys[owner]);
			}
			// Keep the query-owned Property cursor alive across adjacent candidate
			// batches. This avoids restarting the Property Directory root descent for
			// every range batch while preserving the same snapshot and validation fence.
			active.propertyCursor(properties_, snapshot).readEntityPropertiesAndCountsOptionalSorted(
					snapshot, missingKeys, propertyTokens, missingEntries, missingCounts, admission, transientBufferAdmission ? &admitNested : nullptr);
			for (size_t index = 0U; index < missingOwners.size(); ++index) {
				const size_t owner = missingOwners[index];
				counts[owner] = missingCounts[index];
				for (size_t token = 0U; token < propertyTokens.size(); ++token) {
					auto &entry = missingEntries[index * propertyTokens.size() + token];
					std::optional<PropertyValue> value =
							entry.has_value() ? std::optional<PropertyValue>{std::move(entry->value)} : std::nullopt;
					active.putProperty(false, nodeIds[owner], propertyTokens[token], value);
					columns[token][owner] = std::move(value);
				}
			}
		}
		stagingBytes = baseStagingBytes;
		admitStaging(0U);
		if (!countOnlyOwners.empty()) {
			std::vector<EntityPropertyKey> countKeys;
			std::vector<std::optional<size_t>> cachedCounts;
			reserveStaging(countKeys, countOnlyOwners.size());
			reserveStaging(cachedCounts, countOnlyOwners.size(), true);
			for (const size_t owner: countOnlyOwners) {
				countKeys.push_back(keys[owner]);
			}
			active.propertyCursor(properties_, snapshot).readEntityPropertyCountsSorted(snapshot, countKeys, cachedCounts, transientBufferAdmission ? &admitNested : nullptr);
			for (size_t index = 0U; index < countOnlyOwners.size(); ++index) {
				counts[countOnlyOwners[index]] = cachedCounts[index];
			}
		}
		stagingBytes = baseStagingBytes - keys.capacity() * sizeof(EntityPropertyKey)
			- missingOwners.capacity() * sizeof(size_t) - countOnlyOwners.capacity() * sizeof(size_t);
		std::vector<EntityPropertyKey>().swap(keys);
		std::vector<size_t>().swap(missingOwners);
		std::vector<size_t>().swap(countOnlyOwners);
		admitStaging(0U);
		return {std::move(columns), std::move(counts)};
	}

	size_t PageBackedGraphReadAdapter::nodePropertyCount(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, NativeReadSession *session) const {
		if (nodeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed Node Property count requires a nonzero Node ID");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		const std::array<EntityPropertyKey, 1U> keys{{{EntityPropertyOwnerKind::EPOK_NODE, nodeId}}};
		std::array<std::optional<size_t>, 1U> counts;
		activeSession.propertyCursor(properties_, snapshot).readEntityPropertyCountsSorted(snapshot, keys, counts);
		return counts.front().value_or(0U);
	}

	std::vector<std::optional<size_t>> PageBackedGraphReadAdapter::nodePropertyCountsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		std::vector<EntityPropertyKey> keys;
		keys.reserve(nodeIds.size());
		uint64_t previousId = 0U;
		for (const uint64_t nodeId: nodeIds) {
			if (nodeId == 0U || nodeId <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed sorted Node Property counts require strictly increasing Node IDs");
			}
			previousId = nodeId;
			keys.push_back({EntityPropertyOwnerKind::EPOK_NODE, nodeId});
		}
		std::vector<std::optional<size_t>> result(nodeIds.size());
	activeSession.propertyCursor(properties_, snapshot).readEntityPropertyCountsSorted(snapshot, keys, result);
		return result;
	}

	bool PageBackedGraphReadAdapter::nodeHasLabelTokens(const MetadataSnapshotPin &snapshot, const uint64_t nodeId,
													 const std::span<const uint64_t> labelTokens,
													 NativeReadSession *session) const {
		if (nodeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed Node label test requires a nonzero Node ID");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		const NodeEntityRecord record = readNodeRecord(snapshot, nodeId, &activeSession);
		if (labelTokens.empty()) {
			return true;
		}
		if (record.label_overflow.isEmpty()) {
			for (const uint64_t token: labelTokens) {
				if (!labels_.containsNodeLabel(snapshot, record, token)) {
					return false;
				}
			}
			return true;
		}
		const std::vector<uint64_t> actual = readNodeLabelTokens(snapshot, nodeId, &activeSession);
		for (const uint64_t token: labelTokens) {
			if (token == 0U || std::ranges::find(actual, token) == actual.end()) {
				return false;
			}
		}
		return true;
	}

	std::vector<bool> PageBackedGraphReadAdapter::readNodeLabelMatchesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const std::span<const uint64_t> labelTokens, NativeReadSession *session,
			const SelectedPropertyReadAdmission *entityBufferAdmission) const {
		if (nodeIds.empty()) {
			return {};
		}
		if (std::ranges::any_of(nodeIds, [](const uint64_t id) { return id == 0U; }) ||
			!std::ranges::is_sorted(nodeIds) || std::ranges::adjacent_find(nodeIds) != nodeIds.end()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed sorted Node label read requires strictly increasing Node IDs");
		}
		if (std::ranges::any_of(labelTokens, [](const uint64_t token) { return token == 0U; }) ||
			!std::ranges::is_sorted(labelTokens) ||
			std::ranges::adjacent_find(labelTokens) != labelTokens.end()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed sorted Node label read requires ordered unique nonzero label tokens");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		size_t entityBytes = 0U;
		const auto admitEntity = [&](size_t extra) {
			if (extra > std::numeric_limits<size_t>::max() - entityBytes)
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "label Entity staging overflowed");
			if (entityBufferAdmission) (*entityBufferAdmission)(entityBytes + extra);
		};
		if (nodeIds.size() > std::numeric_limits<size_t>::max() / sizeof(uint64_t))
			throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "label missing IDs overflowed");
		// A word per requested bit safely covers implementation-specific block
		// rounding before vector<bool> exposes its actual capacity.
		admitEntity(nodeIds.size() * sizeof(uint64_t));
		std::vector<bool> result(nodeIds.size(), false);
		const size_t resultBytes = result.capacity() / 8U + (result.capacity() % 8U != 0U ? 1U : 0U);
		admitEntity(resultBytes);
		entityBytes += resultBytes;
		std::vector<uint64_t> missingIds;
		admitEntity(nodeIds.size() * sizeof(uint64_t));
		missingIds.reserve(nodeIds.size());
		admitEntity(missingIds.capacity() * sizeof(uint64_t));
		entityBytes += missingIds.capacity() * sizeof(uint64_t);
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			// Label-only cache entries are not sufficient here: the batch result
			// must retain an authenticated entity record for every requested node.
			if (!active.node(nodeIds[index]).has_value()) {
				missingIds.push_back(nodeIds[index]);
			}
		}
		auto records =
				missingIds.empty() ? std::vector<std::optional<NodeEntityRecord>>{} : entities_.readNodesSorted(snapshot, missingIds, entityBufferAdmission ? admitEntity : SelectedPropertyReadAdmission{});
		admitEntity(records.capacity() * sizeof(std::optional<NodeEntityRecord>));
		entityBytes += records.capacity() * sizeof(std::optional<NodeEntityRecord>);
		if (records.size() != missingIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed sorted Node label read changed record cardinality");
		}
		// Keep label evaluation independent from the bounded session cache. A
		// caller may submit more IDs than the cache can retain, and falling back to
		// one scalar Entity lookup per evicted ID defeats the sorted batch contract.
		if (nodeIds.size() > std::numeric_limits<size_t>::max() / sizeof(NodeEntityRecord))
			throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "label resolved records overflowed");
		admitEntity(nodeIds.size() * sizeof(NodeEntityRecord));
		std::vector<NodeEntityRecord> resolved(nodeIds.size());
		admitEntity(resolved.capacity() * sizeof(NodeEntityRecord));
		entityBytes += resolved.capacity() * sizeof(NodeEntityRecord);
		admitEntity(nodeIds.size() * sizeof(uint8_t));
		std::vector<uint8_t> resolvedFlags(nodeIds.size(), 0U);
		admitEntity(resolvedFlags.capacity() * sizeof(uint8_t));
		entityBytes += resolvedFlags.capacity() * sizeof(uint8_t);
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			if (const std::optional<NodeEntityRecord> cached = active.node(nodeIds[index]); cached.has_value()) {
				resolved[index] = *cached;
				resolvedFlags[index] = true;
			}
		}
		for (size_t index = 0U; index < missingIds.size(); ++index) {
			if (!records[index].has_value()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed sorted Node label read found a missing Entity Map record");
			}
			const auto ordinal = std::ranges::lower_bound(nodeIds, missingIds[index]);
			if (ordinal == nodeIds.end() || *ordinal != missingIds[index]) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed sorted Node label read lost its logical ID alignment");
			}
			const size_t resultIndex = static_cast<size_t>(ordinal - nodeIds.begin());
			resolved[resultIndex] = *records[index];
			resolvedFlags[resultIndex] = true;
			active.putNode(missingIds[index], *records[index]);
		}
		for (const bool resolvedFlag : resolvedFlags) {
			if (!resolvedFlag) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
						"page-backed sorted Node label read lost an authenticated Entity record");
			}
		}
		const size_t releasedBytes = missingIds.capacity() * sizeof(uint64_t) +
			records.capacity() * sizeof(std::optional<NodeEntityRecord>);
		std::vector<uint64_t>().swap(missingIds);
		std::vector<std::optional<NodeEntityRecord>>().swap(records);
		entityBytes -= releasedBytes;
		admitEntity(0U);
		if (labelTokens.empty()) {
			std::fill(result.begin(), result.end(), true);
			std::vector<NodeEntityRecord>().swap(resolved);
			std::vector<uint8_t>().swap(resolvedFlags);
			entityBytes = resultBytes; admitEntity(0U);
			return result;
		}
		const auto matchesLabels = [&](const std::span<const uint64_t> actual) {
			return std::ranges::all_of(labelTokens, [&](const uint64_t token) {
				return std::ranges::binary_search(actual, token);
			});
		};
		std::vector<NodeEntityRecord> uncachedRecords;
		std::vector<size_t> uncachedOrdinals;
		const size_t overflowCount = static_cast<size_t>(std::ranges::count_if(resolved, [](const NodeEntityRecord &record) {
			return !record.label_overflow.isEmpty();
		}));
		admitEntity(overflowCount * sizeof(NodeEntityRecord));
		uncachedRecords.reserve(overflowCount);
		admitEntity(uncachedRecords.capacity() * sizeof(NodeEntityRecord));
		entityBytes += uncachedRecords.capacity() * sizeof(NodeEntityRecord);
		admitEntity(overflowCount * sizeof(size_t));
		uncachedOrdinals.reserve(overflowCount);
		admitEntity(uncachedOrdinals.capacity() * sizeof(size_t));
		entityBytes += uncachedOrdinals.capacity() * sizeof(size_t);
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			if (const auto cachedLabels = active.nodeLabelsView(nodeIds[index]); cachedLabels.has_value()) {
				result[index] = matchesLabels(*cachedLabels);
			} else if (resolved[index].label_overflow.isEmpty()) {
				result[index] = std::ranges::all_of(labelTokens, [&](const uint64_t token) {
					return labels_.containsNodeLabel(snapshot, resolved[index], token);
				});
			} else {
				uncachedRecords.push_back(resolved[index]);
				uncachedOrdinals.push_back(index);
			}
		}
		if (!uncachedRecords.empty()) {
			const std::vector<std::vector<uint64_t>> batchLabels =
				labels_.readNodeLabelsSorted(snapshot, uncachedRecords, entityBufferAdmission ? admitEntity : SelectedPropertyReadAdmission{});
			if (batchLabels.size() != uncachedOrdinals.size()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed sorted Node label read changed decoded batch cardinality");
			}
			for (size_t index = 0U; index < uncachedOrdinals.size(); ++index) {
				const size_t ordinal = uncachedOrdinals[index];
				result[ordinal] = matchesLabels(batchLabels[index]);
				active.putNodeLabels(nodeIds[ordinal], batchLabels[index]);
			}
		}
		std::vector<NodeEntityRecord>().swap(uncachedRecords);
		std::vector<size_t>().swap(uncachedOrdinals);
		std::vector<NodeEntityRecord>().swap(resolved);
		std::vector<uint8_t>().swap(resolvedFlags);
		entityBytes = resultBytes; admitEntity(0U);
		return result;
	}

	std::optional<PropertyValue> PageBackedGraphReadAdapter::readEdgeProperty(
			const MetadataSnapshotPin &snapshot, const uint64_t edgeId, const uint32_t propertyToken,
			NativeReadSession *session) const {
		if (edgeId == 0U || propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed Edge Property read requires nonzero identifiers");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (const auto cached = activeSession.property(true, edgeId, propertyToken); cached.has_value()) {
			return *cached;
		}
		// Reuse an authenticated negative presence result within this snapshot;
		// this keeps repeated missing-field expressions off the Property pages.
		if (const auto present = activeSession.propertyPresence(true, edgeId, propertyToken);
			present.has_value() && !*present) {
			activeSession.putProperty(true, edgeId, propertyToken, std::nullopt);
			return std::nullopt;
		}
		const auto entry = activeSession.propertyCursor(properties_, snapshot).readEntityPropertyOptional(
				snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId}, propertyToken);
		const std::optional<PropertyValue> value = entry.has_value() ? std::optional<PropertyValue>{entry->value} : std::nullopt;
		activeSession.putProperty(true, edgeId, propertyToken, value);
		return value;
	}

	bool PageBackedGraphReadAdapter::edgePropertyExists(const MetadataSnapshotPin &snapshot, const uint64_t edgeId,
																		 const uint32_t propertyToken, NativeReadSession *session, const bool requireNonNull) const {
		if (edgeId == 0U || propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
								"page-backed Edge Property presence read requires nonzero identifiers");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (const auto cachedValue = activeSession.property(true, edgeId, propertyToken); cachedValue.has_value()) {
			const bool present = cachedValue->has_value();
			if (requireNonNull) return present && !std::holds_alternative<std::monostate>((*cachedValue)->getVariant());
			activeSession.putPropertyPresence(true, edgeId, propertyToken, present);
			return present;
		}
		if (const auto cached = activeSession.propertyPresence(true, edgeId, propertyToken); cached.has_value()) {
			if (!requireNonNull || !*cached) return *cached;
		}
		const bool present = activeSession.propertyCursor(properties_, snapshot).readEntityPropertyExists(
				snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId}, propertyToken, requireNonNull);
		if (!requireNonNull || present) activeSession.putPropertyPresence(true, edgeId, propertyToken, present);
		return present;
	}

	std::vector<std::optional<PropertyValue>> PageBackedGraphReadAdapter::readEdgePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			const uint32_t propertyToken, NativeReadSession *session) const {
		if (propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed sorted Edge Property read requires a nonzero Property token");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		std::vector<std::optional<PropertyValue>> result(edgeIds.size());
		std::vector<uint64_t> missingIds;
		std::vector<size_t> missingOrdinals;
		missingIds.reserve(edgeIds.size());
		missingOrdinals.reserve(edgeIds.size());
		uint64_t previousId = 0U;
		for (size_t index = 0U; index < edgeIds.size(); ++index) {
			if (edgeIds[index] == 0U || edgeIds[index] <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed sorted Edge Property read requires strictly increasing Edge IDs");
			}
			previousId = edgeIds[index];
			if (const auto cached = active.property(true, edgeIds[index], propertyToken); cached.has_value()) {
				result[index] = *cached;
			} else if (const auto present = active.propertyPresence(true, edgeIds[index], propertyToken);
					present.has_value() && !*present) {
				result[index] = std::nullopt;
			} else {
				missingIds.push_back(edgeIds[index]);
				missingOrdinals.push_back(index);
			}
		}
		if (missingIds.empty()) {
			return result;
		}
		std::vector<EntityPropertyKey> keys;
		keys.reserve(missingIds.size());
		for (const uint64_t edgeId: missingIds) {
			keys.push_back({EntityPropertyOwnerKind::EPOK_EDGE, edgeId});
		}
		std::vector<std::optional<PropertyCollectionRootEntry>> entries(keys.size());
		const std::array<uint32_t, 1U> selectedTokens{propertyToken};
		active.propertyCursor(properties_, snapshot).readEntityPropertiesOptionalSorted(
				snapshot, keys, selectedTokens, entries, false);
		for (size_t index = 0U; index < entries.size(); ++index) {
			const std::optional<PropertyValue> value = entries[index].has_value()
					? std::optional<PropertyValue>{entries[index]->value} : std::nullopt;
			result[missingOrdinals[index]] = value;
			active.putProperty(true, missingIds[index], propertyToken, value);
		}
		return result;
	}

	std::vector<std::vector<std::optional<PropertyValue>>>
	PageBackedGraphReadAdapter::readEdgePropertyColumnsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			const std::span<const uint32_t> propertyTokens, NativeReadSession *session,
			const SelectedPropertyReadAdmission *admission) const {
		if (propertyTokens.empty()) {
			return {};
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		if (edgeIds.size() > std::numeric_limits<size_t>::max() / propertyTokens.size()) {
			throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW,
							  "page-backed Edge Property column read result size overflows");
		}
		uint64_t previousId = 0U;
		uint32_t previousToken = 0U;
		for (const uint32_t token: propertyTokens) {
			if (token == 0U || token <= previousToken) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed Edge Property columns require strictly increasing nonzero tokens");
			}
			previousToken = token;
		}
		for (size_t owner = 0U; owner < edgeIds.size(); ++owner) {
			if (edgeIds[owner] == 0U || edgeIds[owner] <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed Edge Property columns require strictly increasing Edge IDs");
			}
			previousId = edgeIds[owner];
		}
		std::vector<std::vector<std::optional<PropertyValue>>> columns;
		columns.reserve(propertyTokens.size());
		for (size_t token = 0U; token < propertyTokens.size(); ++token) columns.emplace_back(edgeIds.size());
		const auto needsDecode = [&](uint64_t id) {
			return std::ranges::any_of(propertyTokens, [&](uint32_t token) {
				if (active.hasProperty(true, id, token)) return false;
				const auto present = active.propertyPresence(true, id, token);
				return !present.has_value() || *present;
			});
		};
		const size_t missingCount = static_cast<size_t>(std::ranges::count_if(edgeIds, needsDecode));
		std::vector<uint64_t> missingIds;
		missingIds.reserve(missingCount);
		debug::PerfTrace::addValue("native_graph.read.property_column_gather_capacity", static_cast<int64_t>(missingIds.capacity()));
		for (size_t owner = 0U; owner < edgeIds.size(); ++owner) {
			// If any token needs decoding, this owner is read as one rectangular
			// token batch. Avoid copying/admitting cache cells that would then be
			// decoded again and overwritten by that same immutable owner read.
			const bool missing = needsDecode(edgeIds[owner]);
			if (missing) {
				missingIds.push_back(edgeIds[owner]);
				continue;
			}
			for (size_t tokenIndex = 0U; tokenIndex < propertyTokens.size(); ++tokenIndex) {
				const uint32_t token = propertyTokens[tokenIndex];
				if (auto cached = active.property(true, edgeIds[owner], token, admission); cached.has_value()) {
					columns[tokenIndex][owner] = std::move(*cached);
				} else if (const auto present = active.propertyPresence(true, edgeIds[owner], token);
						present.has_value() && !*present) {
					columns[tokenIndex][owner] = std::nullopt;
				}
			}
		}
		if (!missingIds.empty()) {
			std::vector<EntityPropertyKey> missingKeys;
			missingKeys.reserve(missingIds.size());
			for (const uint64_t edgeId: missingIds) {
				missingKeys.push_back({EntityPropertyOwnerKind::EPOK_EDGE, edgeId});
			}
			std::vector<std::optional<PropertyCollectionRootEntry>> entries(
				missingIds.size() * propertyTokens.size());
			active.propertyCursor(properties_, snapshot).readEntityPropertiesOptionalSorted(
					snapshot, missingKeys, propertyTokens, entries, true, admission);
			size_t outputOwner = 0U;
			for (size_t owner = 0U; owner < missingIds.size(); ++owner) {
				while (edgeIds[outputOwner] < missingIds[owner]) ++outputOwner;
				for (size_t token = 0U; token < propertyTokens.size(); ++token) {
					auto &entry = entries[owner * propertyTokens.size() + token];
					std::optional<PropertyValue> value = entry.has_value()
							? std::optional<PropertyValue>{std::move(entry->value)} : std::nullopt;
					// The session keeps its own snapshot-local copy; the decoded owner
					// transfers its payload into the returned column.
					active.putProperty(true, missingIds[owner], propertyTokens[token], value);
					columns[token][outputOwner] = std::move(value);
				}
			}
		}
		return columns;
	}

	void PageBackedGraphReadAdapter::validateNodeExactProperties(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, const uint64_t labelToken,
			const std::span<const uint32_t> propertyTokens, const std::span<const PropertyValue> expectedValues,
			const size_t maximumPropertyEntries, NativeReadSession *session) const {
		const std::array<uint64_t, 1U> nodeIds{nodeId};
		validateNodeExactPropertiesSorted(snapshot, nodeIds, labelToken, propertyTokens, expectedValues,
										 maximumPropertyEntries, session);
	}

	void PageBackedGraphReadAdapter::validateNodeExactPropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds, const uint64_t labelToken,
			const std::span<const uint32_t> propertyTokens, const std::span<const PropertyValue> expectedValues,
			const size_t maximumPropertyEntries, NativeReadSession *session,
			const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *transientBufferAdmission) const {
		if (nodeIds.empty() || labelToken == 0U || maximumPropertyEntries == 0U ||
			propertyTokens.empty() || propertyTokens.size() != expectedValues.size() ||
			propertyTokens.size() > maximumPropertyEntries ||
			nodeIds.size() > std::numeric_limits<size_t>::max() / propertyTokens.size()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed exact Node batch verification has invalid identifiers or Property bounds");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		size_t stagingBytes = 0U;
		const auto admitStaging = [&](size_t bytes) {
			if (bytes > std::numeric_limits<size_t>::max() - stagingBytes)
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "exact verification staging overflowed");
			if (transientBufferAdmission) (*transientBufferAdmission)(stagingBytes + bytes);
		};
		const auto reserveStaging = [&](auto &values, size_t count, bool resize = false) {
			using Value = typename std::remove_reference_t<decltype(values)>::value_type;
			if (count > std::numeric_limits<size_t>::max() / sizeof(Value))
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "exact verification slots overflowed");
			admitStaging(count * sizeof(Value));
			values.reserve(count);
			admitStaging(values.capacity() * sizeof(Value));
			stagingBytes += values.capacity() * sizeof(Value);
			if (resize) values.resize(count);
		};
		const SelectedPropertyReadAdmission admitNested = [&](size_t bytes) { admitStaging(bytes); };

		std::vector<size_t> predicateOrdinals;
		reserveStaging(predicateOrdinals, propertyTokens.size(), true);
		for (size_t index = 0U; index < predicateOrdinals.size(); ++index) {
			if (propertyTokens[index] == 0U || expectedValues[index].getType() == PropertyType::NULL_TYPE) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed exact Node batch verification has an invalid Property predicate");
			}
			predicateOrdinals[index] = index;
		}
		std::ranges::sort(predicateOrdinals, [propertyTokens](const size_t left, const size_t right) {
			return propertyTokens[left] < propertyTokens[right];
		});
		std::vector<uint32_t> sortedTokens;
		reserveStaging(sortedTokens, propertyTokens.size());
		for (const size_t ordinal: predicateOrdinals) {
			if (!sortedTokens.empty() && sortedTokens.back() == propertyTokens[ordinal]) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed exact Node batch verification has duplicate Property tokens");
			}
			sortedTokens.push_back(propertyTokens[ordinal]);
		}

		std::vector<EntityPropertyKey> keys;
		reserveStaging(keys, nodeIds.size());
		uint64_t previousNodeId = 0U;
		for (const uint64_t nodeId: nodeIds) {
			if (nodeId == 0U || nodeId <= previousNodeId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed exact Node batch verification requires strictly increasing Node IDs");
			}
			previousNodeId = nodeId;
			keys.push_back({EntityPropertyOwnerKind::EPOK_NODE, nodeId});
		}
		// Candidate verification is already a sorted batch. Resolve the Entity Map
		// once and group records by Entity page instead of descending the directory
		// independently for every candidate.
		if (nodeIds.size() > std::numeric_limits<size_t>::max() / sizeof(std::optional<NodeEntityRecord>))
			throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "exact Node record slots overflowed");
		admitStaging(nodeIds.size() * sizeof(std::optional<NodeEntityRecord>));
		auto records = entities_.readNodesSorted(snapshot, nodeIds, transientBufferAdmission ? admitNested : SelectedPropertyReadAdmission{});
		admitStaging(records.capacity() * sizeof(std::optional<NodeEntityRecord>));
		stagingBytes += records.capacity() * sizeof(std::optional<NodeEntityRecord>);
		if (records.size() != nodeIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed exact Node batch verification changed Entity record cardinality");
		}
		for (size_t index = 0U; index < records.size(); ++index) {
			if (!records[index].has_value()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed exact Node batch verification found a missing Entity record");
			}
			const NodeEntityRecord &record = *records[index];
			activeSession.putNode(nodeIds[index], record);
			if (!labels_.containsNodeLabel(snapshot, record, labelToken, transientBufferAdmission ? admitNested : SelectedPropertyReadAdmission{})) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed exact Node batch verification found a candidate outside the indexed label");
			}
		}
		// Count and selected-value validation share the same owner-keyed directory
		// location. Resolve both in one merge so digest-collision candidates do not
		// reopen the Property Directory or page route for the same root.
		std::vector<std::optional<size_t>> propertyCounts;
		reserveStaging(propertyCounts, keys.size(), true);
		std::vector<std::optional<PropertyCollectionRootEntry>> selected;
		reserveStaging(selected, keys.size() * sortedTokens.size(), true);
		activeSession.propertyCursor(properties_, snapshot).readEntityPropertiesAndCountsOptionalSorted(
				snapshot, keys, sortedTokens, selected, propertyCounts, admission, transientBufferAdmission ? &admitNested : nullptr);
		for (const std::optional<size_t> count: propertyCounts) {
			if (!count.has_value() || *count > maximumPropertyEntries) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed exact Node batch verification exceeded the Property entry bound");
			}
		}
		for (size_t owner = 0U; owner < keys.size(); ++owner) {
			for (size_t token = 0U; token < sortedTokens.size(); ++token) {
				const auto &entry = selected[owner * sortedTokens.size() + token];
				if (!entry.has_value() || entry->value != expectedValues[predicateOrdinals[token]]) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed exact Node batch verification found a stale indexed Property");
				}
			}
		}
		std::vector<size_t>().swap(predicateOrdinals);
		std::vector<uint32_t>().swap(sortedTokens);
		std::vector<EntityPropertyKey>().swap(keys);
		std::vector<std::optional<NodeEntityRecord>>().swap(records);
		std::vector<std::optional<size_t>>().swap(propertyCounts);
		std::vector<std::optional<PropertyCollectionRootEntry>>().swap(selected);
		if (transientBufferAdmission) (*transientBufferAdmission)(0U);
	}

	PageBackedGraphEdge PageBackedGraphReadAdapter::readEdge(const MetadataSnapshotPin &snapshot,
															 uint64_t edgeId, NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (edgeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Edge read requires a nonzero logical Edge ID");
		}
		// EntityPageStore resolves EdgeId through the snapshot-pinned Entity Map.
		const std::optional<EdgeEntityRecord> cached = activeSession.edge(edgeId);
		debug::PerfTrace::addValue(cached.has_value() ? "native_graph.read.entity_edge_record_cache_hit"
															 : "native_graph.read.entity_edge_record_cache_miss",
													 1);
		const EdgeEntityRecord record = cached.has_value() ? *cached : entities_.readEdge(snapshot, edgeId);
		if (!cached.has_value()) {
			activeSession.putEdge(edgeId, record);
		}
		// Keep complete Edge reads on the same bounded Property cursor as selected
		// reads so repeated materialization does not redo the directory descent.
		return materializeEdge(snapshot, edgeId, record, &activeSession.propertyCursor(properties_, snapshot));
	}

	EdgeEntityRecord PageBackedGraphReadAdapter::readEdgeTopology(const MetadataSnapshotPin &snapshot,
																			 const uint64_t edgeId, NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (edgeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Edge topology read requires a nonzero logical Edge ID");
		}
		const std::optional<EdgeEntityRecord> cached = activeSession.edge(edgeId);
		debug::PerfTrace::addValue(cached.has_value() ? "native_graph.read.entity_edge_record_cache_hit"
															 : "native_graph.read.entity_edge_record_cache_miss",
													 1);
		const EdgeEntityRecord record = cached.has_value() ? *cached : entities_.readEdge(snapshot, edgeId);
		if (!cached.has_value()) {
			activeSession.putEdge(edgeId, record);
		}
		return record;
	}

	std::vector<PropertyCollectionRootEntry> PageBackedGraphReadAdapter::readEdgeProperties(
			const MetadataSnapshotPin &snapshot, const uint64_t edgeId, NativeReadSession *session, const SelectedPropertyReadAdmission *admission) const {
		if (edgeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed Edge Property root read requires a nonzero logical Edge ID");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		return readProperties(snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId},
																										 &active.propertyCursor(properties_, snapshot), admission);
	}

	std::vector<std::vector<PropertyCollectionRootEntry>> PageBackedGraphReadAdapter::readEdgePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			NativeReadSession *session, const SelectedPropertyReadAdmission *admission) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		uint64_t previousId = 0U;
		for (const uint64_t id : edgeIds) {
			if (id == 0U || id <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
					"page-backed sorted Edge Property root read requires strictly increasing Edge IDs");
			}
			previousId = id;
		}
		std::vector<std::vector<PropertyCollectionRootEntry>> result(edgeIds.size());
		PropertyCollectionRootScanCursor &cursor = active.propertyCursor(properties_, snapshot);
		for (size_t index = 0U; index < edgeIds.size(); ++index) {
			const uint64_t edgeId = edgeIds[index];
			if (auto root = cursor.readEntityOptional(snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId}, admission);
				root.has_value()) {
				result[index] = std::move(root->entries);
			}
		}
		return result;
	}

	std::vector<EdgeEntityRecord> PageBackedGraphReadAdapter::readEdgeTopologiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			NativeReadSession *session) const {
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		if (edgeIds.empty()) {
			return {};
		}
		uint64_t previousEdgeId = 0U;
		std::vector<uint64_t> missingIds;
		missingIds.reserve(edgeIds.size());
		for (const uint64_t edgeId: edgeIds) {
			if (edgeId == 0U || edgeId <= previousEdgeId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed shaped Edge batch requires strictly increasing Edge IDs");
			}
			previousEdgeId = edgeId;
			if (!activeSession.edge(edgeId).has_value()) {
				missingIds.push_back(edgeId);
			}
		}
		const std::vector<std::optional<EdgeEntityRecord>> missing =
				missingIds.empty() ? std::vector<std::optional<EdgeEntityRecord>>{}
										  : entities_.readEdgesSorted(snapshot, missingIds);
		if (missing.size() != missingIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed shaped Edge batch changed Entity record cardinality");
		}
		// The session cache is deliberately bounded and is not a safe backing store
		// for a result larger than that bound. Preserve every resolved record in the
		// batch-local result while admitting it to the cache opportunistically.
		std::vector<EdgeEntityRecord> resolved(edgeIds.size());
		std::vector<bool> resolvedFlags(edgeIds.size(), false);
		for (size_t index = 0U; index < edgeIds.size(); ++index) {
			if (const std::optional<EdgeEntityRecord> cached = activeSession.edge(edgeIds[index]); cached.has_value()) {
				resolved[index] = *cached;
				resolvedFlags[index] = true;
			}
		}
		for (size_t index = 0U; index < missing.size(); ++index) {
			if (!missing[index].has_value()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed shaped Edge batch found a missing Entity record");
			}
			const auto ordinal = std::ranges::lower_bound(edgeIds, missingIds[index]);
			if (ordinal == edgeIds.end() || *ordinal != missingIds[index]) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed shaped Edge batch lost its logical ID alignment");
			}
			const size_t resultIndex = static_cast<size_t>(ordinal - edgeIds.begin());
			resolved[resultIndex] = *missing[index];
			resolvedFlags[resultIndex] = true;
			activeSession.putEdge(missingIds[index], *missing[index]);
		}
		for (const bool resolvedFlag: resolvedFlags) {
			if (!resolvedFlag) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed shaped Edge batch lost an authenticated Entity record");
			}
		}
		return resolved;
	}

	bool
	PageBackedGraphReadAdapter::visitNodesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
																		const std::function<bool(const PageBackedGraphNode &)> &visitor,
																		NativeReadSession *session) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Node scan requires a nonzero record bound and visitor");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		// Complete scans share the query-owned cursor with selected reads. This
		// avoids reopening the Property Directory when a scan is followed by a
		// projection or when adjacent entities share a Property page.
		PropertyCollectionRootScanCursor &propertyCursor = activeSession.propertyCursor(properties_, snapshot);
		return entities_.visitNodesUntil(
				snapshot, maximumRecords,
				[this, &snapshot, &visitor, &propertyCursor, &activeSession](uint64_t nodeId, const NodeEntityRecord &record) {
					// Keep the authenticated fixed record available to later result or
					// binding materialization in this snapshot-owned session.
					activeSession.putNode(nodeId, record);
					return visitor(materializeNode(snapshot, nodeId, record, &propertyCursor));
				}, &activeSession);
	}

	bool PageBackedGraphReadAdapter::visitNodeRefsUntil(
			const MetadataSnapshotPin &snapshot, const size_t maximumRecords,
			const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed graph Node reference scan requires a nonzero record bound and visitor");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		return entities_.visitNodesUntil(snapshot, maximumRecords,
													[&visitor, &activeSession](const uint64_t nodeId, const NodeEntityRecord &record) {
														activeSession.putNode(nodeId, record);
														return visitor({nodeId, false, 0U, 0U, 0U});
													}, &activeSession);
	}

	bool PageBackedGraphReadAdapter::visitNodeRefsWithLabelTokenMatchesUntil(
			const MetadataSnapshotPin &snapshot, const size_t maximumRecords, const std::span<const uint64_t> labelTokens,
			const std::function<bool(const EntityRef &, bool)> &visitor, NativeReadSession *session) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed labeled Node reference scan requires a nonzero record bound and visitor");
		}
		if (std::ranges::any_of(labelTokens, [](const uint64_t token) { return token == 0U; }) ||
			!std::ranges::is_sorted(labelTokens) || std::ranges::adjacent_find(labelTokens) != labelTokens.end()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed labeled Node reference scan requires ordered unique nonzero label tokens");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		return entities_.visitNodesUntil(
				snapshot, maximumRecords,
				[this, &snapshot, labelTokens, &visitor, &activeSession](const uint64_t nodeId, const NodeEntityRecord &record) {
					activeSession.putNode(nodeId, record);
					bool labelsMatch = true;
					if (!labelTokens.empty()) {
						if (record.label_overflow.isEmpty()) {
							labelsMatch = std::ranges::all_of(labelTokens, [&](const uint64_t token) {
								return labels_.containsNodeLabel(snapshot, record, token);
							});
						} else {
							auto actual = activeSession.nodeLabels(nodeId);
							if (!actual.has_value()) {
								actual = labels_.readNodeLabels(snapshot, record);
								activeSession.putNodeLabels(nodeId, *actual);
							}
							labelsMatch = std::ranges::all_of(labelTokens, [&](const uint64_t token) {
								return std::ranges::binary_search(*actual, token);
							});
						}
					}
					return visitor({nodeId, false, 0U, 0U, 0U}, labelsMatch);
				}, &activeSession);
	}

	bool
	PageBackedGraphReadAdapter::visitEdgesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
																		const std::function<bool(const PageBackedGraphEdge &)> &visitor,
																		NativeReadSession *session) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Edge scan requires a nonzero record bound and visitor");
		}
		debug::ScopedPerfTimer timer("native_graph.read.edge_native_scan");
		const bool tracing = debug::PerfTrace::isEnabled();
		uint64_t materializationNanoseconds = 0U;
		uint64_t visitorNanoseconds = 0U;
		uint64_t records = 0U;
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		// Keep full Edge materialization on the same bounded session cursor used by
		// selected-column reads, so a query never pays a second Directory setup.
		PropertyCollectionRootScanCursor &propertyCursor = activeSession.propertyCursor(properties_, snapshot);
		const bool exhausted = entities_.visitEdgesUntil(
				snapshot, maximumRecords,
				[this, &snapshot, &visitor, &propertyCursor, tracing, &materializationNanoseconds, &visitorNanoseconds,
				 &records, &activeSession](uint64_t edgeId, const EdgeEntityRecord &record) {
					activeSession.putEdge(edgeId, record);
					const auto materializationStart = tracing ? std::chrono::steady_clock::now()
																	 : std::chrono::steady_clock::time_point{};
					PageBackedGraphEdge edge = materializeEdge(snapshot, edgeId, record, &propertyCursor);
					if (tracing) {
						materializationNanoseconds += elapsedNanoseconds(materializationStart);
					}
					const auto visitorStart = tracing ? std::chrono::steady_clock::now()
																	 : std::chrono::steady_clock::time_point{};
					const bool keepGoing = visitor(edge);
					if (tracing) {
						visitorNanoseconds += elapsedNanoseconds(visitorStart);
						++records;
					}
					return keepGoing;
				}, &activeSession);
		if (tracing) {
			debug::PerfTrace::addDurationBatch("native_graph.read.edge_native_materialization",
											 materializationNanoseconds, records);
			debug::PerfTrace::addDurationBatch("native_graph.read.edge_logical_visitor", visitorNanoseconds, records);
		}
		return exhausted;
	}

	bool PageBackedGraphReadAdapter::visitEdgeRefsUntil(
			const MetadataSnapshotPin &snapshot, const size_t maximumRecords,
			const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed graph Edge reference scan requires a nonzero record bound and visitor");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		return entities_.visitEdgesUntil(snapshot, maximumRecords,
												[&visitor, &activeSession](const uint64_t edgeId, const EdgeEntityRecord &record) {
													activeSession.putEdge(edgeId, record);
												if (record.source_node_id == 0U || record.target_node_id == 0U) {
													throwAdapterError(FormatError::FERR_INVALID_STATE,
															  "page-backed graph Edge reference has invalid endpoints");
												}
												return visitor({edgeId, true, record.source_node_id, record.target_node_id, record.type_id});
												}, &activeSession);
	}

	bool PageBackedGraphReadAdapter::visitEdgeTopologiesUntil(
			const MetadataSnapshotPin &snapshot, size_t maximumRecords,
			const std::function<bool(uint64_t edgeId, const EdgeEntityRecord &)> &visitor,
			NativeReadSession *session) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Edge topology scan requires a nonzero record bound and visitor");
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.edge_topology_scan_calls", 1);
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		return entities_.visitEdgesUntil(snapshot, maximumRecords,
																								 [&visitor, &activeSession](const uint64_t edgeId, const EdgeEntityRecord &record) {
																												activeSession.putEdge(edgeId, record);
																												if (debug::PerfTrace::areValuesEnabled()) {
																													debug::PerfTrace::addValue(
																													"native_graph.read.edge_topology_scan_edges", 1);
																												}
																												return visitor(edgeId, record);
																												}, &activeSession);
	}

	PageBackedGraphNodePage PageBackedGraphReadAdapter::scanNodesAfter(const MetadataSnapshotPin &snapshot,
																														 uint64_t afterNodeId, size_t maximumRecords,
																														 size_t maximumMetadataPages,
																														 NativeReadSession *session) const {
		return scanNodesAfter(snapshot, afterNodeId, maximumRecords, maximumMetadataPages,
																														  EntityReadShape{.include_labels = true, .all_properties = true}, session);
	}

	PageBackedGraphNodePage PageBackedGraphReadAdapter::scanNodesAfter(const MetadataSnapshotPin &snapshot,
																														 uint64_t afterNodeId, size_t maximumRecords,
																														 size_t maximumMetadataPages,
																														 const EntityReadShape &shape,
																														 NativeReadSession *session) const {
		validateEntityReadShape(shape);
		if (maximumRecords == 0U || maximumMetadataPages == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed graph resumable Node scan requires positive bounds");
		}
		PageBackedGraphNodePage result;
		size_t metadataPagesVisited = 0U;
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		std::vector<uint64_t> nodeIds;
		std::vector<NodeEntityRecord> records;
		nodeIds.reserve(maximumRecords);
		records.reserve(maximumRecords);
		const bool exhausted = entities_.visitNodesAfter(
				snapshot, afterNodeId, maximumRecords, maximumMetadataPages,
				[&nodeIds, &records, &activeSession](uint64_t nodeId, const NodeEntityRecord &record) {
					if (!nodeIds.empty() && nodeId <= nodeIds.back()) {
						throwAdapterError(FormatError::FERR_INVALID_STATE,
								"page-backed Node scan returned non-monotonic logical IDs");
					}
					nodeIds.push_back(nodeId);
					records.push_back(record);
					activeSession.putNode(nodeId, record);
					return true;
				},
				&metadataPagesVisited);
		if (nodeIds.size() != records.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed Node scan changed its fixed-record cardinality");
		}
		const std::vector<std::vector<uint64_t>> labelSets = shape.include_labels
				? labels_.readNodeLabelsSorted(snapshot, records)
				: std::vector<std::vector<uint64_t>>(nodeIds.size());
		if (labelSets.size() != nodeIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
									"page-backed Node scan changed its label cardinality");
		}
		std::vector<std::vector<PropertyCollectionRootEntry>> propertySets(nodeIds.size());
		if (shape.all_properties) {
			propertySets = readNodePropertiesSorted(snapshot, nodeIds, &activeSession);
		} else {
			// Gather all requested fields in one owner/token batch rather than
			// reopening the same Property owner separately for every token.
			auto columns = readNodePropertyColumnsSorted(snapshot, nodeIds, shape.property_tokens, &activeSession);
			if (columns.size() != shape.property_tokens.size()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE, "page-backed selected Node scan changed its column cardinality");
			}
			for (size_t column = 0U; column < columns.size(); ++column) {
				if (columns[column].size() != nodeIds.size()) {
					throwAdapterError(FormatError::FERR_INVALID_STATE, "page-backed selected Node scan changed its Property cardinality");
				}
				for (size_t index = 0U; index < nodeIds.size(); ++index) {
					if (columns[column][index]) {
						propertySets[index].push_back({shape.property_tokens[column], std::move(*columns[column][index])});
					}
				}
			}
		}
		if (propertySets.size() != nodeIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
									"page-backed Node scan changed its Property cardinality");
		}
		result.nodes.reserve(nodeIds.size());
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			result.nodes.push_back({nodeIds[index], labelSets[index], propertySets[index]});
		}
		result.may_have_more = !exhausted;
		result.metadata_pages_visited = metadataPagesVisited;
		return result;
	}

	PageBackedGraphAdjacencyPage PageBackedGraphReadAdapter::scanAdjacentEdges(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
			std::optional<PageBackedGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			NativeReadSession *session, const SelectedPropertyReadAdmission *admission,
			const std::function<void(size_t)> &retainedBufferAdmission) const {
		return scanAdjacentEdges(snapshot, nodeId, direction, continuation, maximumRecords, nullptr, session, admission, retainedBufferAdmission);
	}

	PageBackedGraphAdjacencyPage PageBackedGraphReadAdapter::scanAdjacentEdges(
		const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
		std::optional<PageBackedGraphAdjacencyContinuation> continuation, size_t maximumRecords,
		PropertyCollectionRootScanCursor *propertyCursor, NativeReadSession *session, const SelectedPropertyReadAdmission *admission,
			const std::function<void(size_t)> &retainedBufferAdmission) const {
		if (nodeId == 0U || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency scan requires a Node ID and positive record bound");
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &readSession = selectNativeReadSession(snapshot, session, localSession);
		const GlobalAdjacencyDirection adjacencyDirection = globalDirection(direction);
		if (continuation.has_value() && (continuation->previous_edge_id == 0U || continuation->next_edge_id <= continuation->previous_edge_id)) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency continuation has invalid Edge order");
		}

		// The source Node is invariant across all pages of one adjacency scan.
		// Keep its authenticated Entity record in the snapshot-bound session so
		// direct page-at-a-time callers do not repeat an Entity Map lookup.
		if (!readSession.node(nodeId).has_value()) {
			readSession.putNode(nodeId, entities_.readNode(snapshot, nodeId));
		}
		std::optional<GlobalAdjacencyContinuation> adjacencyContinuation;
		if (continuation.has_value()) {
			adjacencyContinuation = {snapshot.superblock().generation, adjacencyDirection, nodeId,
									 continuation->previous_edge_id, snapshot.superblock().database_id};
		}
		const size_t boundedRecords = std::min(maximumRecords, maximum_adjacency_edges_);
		if (boundedRecords == 0U) { // constructor validation keeps both bounds positive.
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency scan has an invalid configured edge bound");
		}
		const auto admitBuffers = [&](size_t base, size_t count, size_t width) {
			if (width != 0U && count > (std::numeric_limits<size_t>::max() - base) / width) {
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "adjacency buffer admission size overflowed");
			}
			const size_t bytes = base + count * width;
			if (retainedBufferAdmission) retainedBufferAdmission(bytes);
			return bytes;
		};
		const GlobalAdjacencyRange edgeIds = adjacency_.scan(snapshot, adjacencyDirection, nodeId, boundedRecords + 1U,
																									 adjacencyContinuation, retainedBufferAdmission);
		if (continuation.has_value() &&
			(edgeIds.edge_ids.empty() || edgeIds.edge_ids.front() != continuation->next_edge_id)) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency continuation does not match the global adjacency range");
		}
		const auto endpointMatches = [nodeId, direction](const EdgeEntityRecord &edge) {
			return direction == PageBackedGraphReadDirection::PBGRD_OUTGOING ? edge.source_node_id == nodeId
																			 : edge.target_node_id == nodeId;
		};
		if (propertyCursor == nullptr) {
			propertyCursor = &readSession.propertyCursor(properties_, snapshot);
		}
		PageBackedGraphAdjacencyPage result;
		const size_t resultCount = std::min(boundedRecords, edgeIds.edge_ids.size());
		const size_t sourceIdsBytes = admitBuffers(0U, edgeIds.edge_ids.capacity(), sizeof(uint64_t));
		admitBuffers(sourceIdsBytes, resultCount, sizeof(PageBackedGraphEdge));
		result.edges.reserve(resultCount);
		const size_t outputBytes = admitBuffers(sourceIdsBytes, result.edges.capacity(), sizeof(PageBackedGraphEdge));
		admitBuffers(outputBytes, resultCount, sizeof(std::optional<EdgeEntityRecord>));
		std::vector<std::optional<EdgeEntityRecord>> cachedRecords(resultCount);
		const size_t cachedBytes = admitBuffers(outputBytes, cachedRecords.capacity(), sizeof(std::optional<EdgeEntityRecord>));
		// Capture hits before inserting misses can evict records needed by this page.
		// Adjacency IDs are returned in logical order. Resolve only cache misses in
		// one sorted Entity-Map/page-local batch, then restore that order while
		// materializing the public page. This removes one Directory descent per Edge
		// without exposing physical locations or changing duplicate/order semantics.
		std::vector<uint64_t> missingEdgeIds;
		admitBuffers(cachedBytes, resultCount, sizeof(uint64_t));
		missingEdgeIds.reserve(resultCount);
		const size_t gatherBytes = admitBuffers(cachedBytes, missingEdgeIds.capacity(), sizeof(uint64_t));
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			const uint64_t edgeId = edgeIds.edge_ids[ordinal];
			cachedRecords[ordinal] = readSession.edge(edgeId);
			if (!cachedRecords[ordinal].has_value()) {
				missingEdgeIds.push_back(edgeId);
			}
		}
		std::ranges::sort(missingEdgeIds);
		missingEdgeIds.erase(std::unique(missingEdgeIds.begin(), missingEdgeIds.end()), missingEdgeIds.end());
		const std::vector<std::optional<EdgeEntityRecord>> missingRecords =
				entities_.readEdgesSorted(snapshot, missingEdgeIds,
				[&](size_t bytes) { admitBuffers(gatherBytes, bytes, 1U); });
		admitBuffers(gatherBytes, missingRecords.capacity(), sizeof(std::optional<EdgeEntityRecord>));
		if (missingRecords.size() != missingEdgeIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency batch changed Edge record cardinality");
		}
		const size_t sourceBufferBytes = admitBuffers(gatherBytes, missingRecords.capacity(), sizeof(std::optional<EdgeEntityRecord>));
		size_t propertySlotsBytes = 0U;
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			const uint64_t edgeId = edgeIds.edge_ids[ordinal];
			std::optional<EdgeEntityRecord> cached = cachedRecords[ordinal];
			if (!cached.has_value()) {
				const auto position = std::ranges::lower_bound(missingEdgeIds, edgeId);
				if (position == missingEdgeIds.end() || *position != edgeId) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
							"page-backed graph adjacency batch lost an Edge ID");
				}
				const size_t missingOrdinal = static_cast<size_t>(position - missingEdgeIds.begin());
				if (!missingRecords[missingOrdinal].has_value()) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
							"page-backed graph adjacency batch references a missing Edge");
				}
				readSession.putEdge(edgeId, *missingRecords[missingOrdinal]);
				cached = missingRecords[missingOrdinal];
			}
			const EdgeEntityRecord &edge = *cached;
			if (!endpointMatches(edge)) { // adjacency and Entity Map records share one pinned publication.
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph adjacency record has an inconsistent endpoint");
			}
			size_t currentSlots = 0U;
			const SelectedPropertyReadAdmission slotsAdmission = [&](size_t bytes) {
				currentSlots = bytes;
				if (propertySlotsBytes > std::numeric_limits<size_t>::max() - sourceBufferBytes) {
					throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "adjacency source slots overflowed");
				}
				admitBuffers(sourceBufferBytes + propertySlotsBytes, bytes, 1U);
			};
			result.edges.push_back(materializeEdge(snapshot, edgeId, edge, propertyCursor, admission, &slotsAdmission));
			if (currentSlots > std::numeric_limits<size_t>::max() - propertySlotsBytes) {
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "adjacency Property slots overflowed");
			}
			propertySlotsBytes += currentSlots;
		}
		if (edgeIds.edge_ids.size() > resultCount) {
			if (result.edges.empty()) { // a continuation requires a positive bounded result count.
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed adjacency scan cannot return a continuation without an Edge");
			}
			result.continuation = PageBackedGraphAdjacencyContinuation{result.edges.back().edge_id,
																												   edgeIds.edge_ids[resultCount]};
		}
		admitBuffers(propertySlotsBytes, result.edges.capacity(), sizeof(PageBackedGraphEdge));
		return result;
	}

	PageBackedGraphAdjacencyTopologyPage PageBackedGraphReadAdapter::scanAdjacentEdgeTopologies(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, const PageBackedGraphReadDirection direction,
			const std::optional<PageBackedGraphAdjacencyContinuation> continuation,
			const size_t maximumRecords, NativeReadSession *session,
			const std::function<void(size_t)> &retainedBufferAdmission) const {
		if (nodeId == 0U || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency topology scan requires a Node ID and positive record bound");
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.adjacency_topology_scan_calls", 1);
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &readSession = selectNativeReadSession(snapshot, session, localSession);
		const GlobalAdjacencyDirection adjacencyDirection = globalDirection(direction);
		if (continuation.has_value() &&
			(continuation->previous_edge_id == 0U || continuation->next_edge_id <= continuation->previous_edge_id)) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency topology continuation has a zero Edge ID");
		}
		if (!readSession.node(nodeId).has_value()) {
			readSession.putNode(nodeId, entities_.readNode(snapshot, nodeId));
		}
		std::optional<GlobalAdjacencyContinuation> adjacencyContinuation;
		if (continuation.has_value()) {
			adjacencyContinuation = {snapshot.superblock().generation, adjacencyDirection, nodeId,
									 continuation->previous_edge_id, snapshot.superblock().database_id};
		}
		const size_t boundedRecords = std::min(maximumRecords, maximum_adjacency_edges_);
		if (boundedRecords == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency topology scan has an invalid configured edge bound");
		}
		const auto admitBuffers = [&](size_t base, size_t count, size_t width) {
			if (width != 0U && count > (std::numeric_limits<size_t>::max() - base) / width) {
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "topology buffer admission size overflowed");
			}
			const size_t bytes = base + count * width;
			if (retainedBufferAdmission) retainedBufferAdmission(bytes);
			return bytes;
		};
		GlobalAdjacencyRange edgeIds =
				adjacency_.scan(snapshot, adjacencyDirection, nodeId, boundedRecords + 1U, adjacencyContinuation, retainedBufferAdmission);
		if (continuation.has_value() &&
			(edgeIds.edge_ids.empty() || edgeIds.edge_ids.front() != continuation->next_edge_id)) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency topology continuation does not match the global adjacency range");
		}
		const auto endpointMatches = [nodeId, direction](const EdgeEntityRecord &edge) {
			return direction == PageBackedGraphReadDirection::PBGRD_OUTGOING ? edge.source_node_id == nodeId
																												 : edge.target_node_id == nodeId;
		};
		PageBackedGraphAdjacencyTopologyPage result;
		const size_t resultCount = std::min(boundedRecords, edgeIds.edge_ids.size());
		const size_t sourceIdsBytes = admitBuffers(0U, edgeIds.edge_ids.capacity(), sizeof(uint64_t));
		const std::span<const uint64_t> requestedEdgeIds(edgeIds.edge_ids.data(), resultCount);
		// Capture cached hits before admitting any new records: insertions may
		// evict later hits when this page exceeds the session cache capacity.
		admitBuffers(sourceIdsBytes, resultCount, sizeof(std::optional<EdgeEntityRecord>));
		std::vector<std::optional<EdgeEntityRecord>> records(resultCount);
		const size_t sourceRecordsBytes = admitBuffers(sourceIdsBytes, records.capacity(), sizeof(std::optional<EdgeEntityRecord>));
		std::vector<uint64_t> missingEdgeIds;
		size_t missingCount = 0U;
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			records[ordinal] = readSession.edge(requestedEdgeIds[ordinal]);
			if (!records[ordinal].has_value()) ++missingCount;
		}
		admitBuffers(sourceRecordsBytes, missingCount, sizeof(uint64_t));
		missingEdgeIds.reserve(missingCount);
		const size_t gatherBytes = admitBuffers(sourceRecordsBytes, missingEdgeIds.capacity(), sizeof(uint64_t));
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			if (!records[ordinal].has_value()) missingEdgeIds.push_back(requestedEdgeIds[ordinal]);
		}
		{
			admitBuffers(gatherBytes, missingCount, sizeof(std::optional<EdgeEntityRecord>));
			const auto missingRecords = entities_.readEdgesSorted(snapshot, missingEdgeIds,
				[&](size_t bytes) { admitBuffers(gatherBytes, bytes, 1U); });
			admitBuffers(gatherBytes, missingRecords.capacity(), sizeof(std::optional<EdgeEntityRecord>));
			if (missingRecords.size() != missingEdgeIds.size()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed graph adjacency topology batch changed its cardinality");
			}
			size_t missingIndex = 0U;
			for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
				if (records[ordinal].has_value()) continue;
				records[ordinal] = missingRecords[missingIndex++];
				if (records[ordinal].has_value()) readSession.putEdge(requestedEdgeIds[ordinal], *records[ordinal]);
			}
		} // Batch staging releases before output publication.
		admitBuffers(gatherBytes, 0U, 0U);
		std::vector<uint64_t>().swap(missingEdgeIds);
		admitBuffers(sourceRecordsBytes, resultCount, sizeof(uint64_t) + sizeof(EdgeEntityRecord));
		result.edge_ids.reserve(resultCount);
		const size_t outputIdsBytes = admitBuffers(sourceRecordsBytes, result.edge_ids.capacity(), sizeof(uint64_t));
		admitBuffers(outputIdsBytes, resultCount, sizeof(EdgeEntityRecord));
		result.edges.reserve(resultCount);
		admitBuffers(outputIdsBytes, result.edges.capacity(), sizeof(EdgeEntityRecord));
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			const uint64_t edgeId = requestedEdgeIds[ordinal];
			if (!records[ordinal].has_value()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed graph adjacency topology references a missing Edge");
			}
			const EdgeEntityRecord edge = *records[ordinal];
			if (!endpointMatches(edge)) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph adjacency topology has an inconsistent endpoint");
			}
			result.edge_ids.push_back(edgeId);
			result.edges.push_back(edge);
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.adjacency_topology_scan_edges",
								static_cast<int64_t>(result.edges.size()));
		}
		if (edgeIds.edge_ids.size() > resultCount) {
			if (result.edge_ids.empty()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed adjacency topology scan cannot return a continuation without an Edge");
			}
			result.continuation = {result.edge_ids.back(), edgeIds.edge_ids[resultCount]};
		}
		std::vector<std::optional<EdgeEntityRecord>>().swap(records);
		std::vector<uint64_t>().swap(edgeIds.edge_ids);
		const size_t finalIdsBytes = result.edge_ids.capacity() * sizeof(uint64_t);
		admitBuffers(finalIdsBytes, result.edges.capacity(), sizeof(EdgeEntityRecord));
		return result;
	}

	PageBackedGraphAdjacencyCursor
	PageBackedGraphReadAdapter::openAdjacentEdgeCursor(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																	PageBackedGraphReadDirection direction,
																	NativeReadSession *session) const {
		if (nodeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency cursor requires a nonzero Node ID");
		}
		if (direction != PageBackedGraphReadDirection::PBGRD_OUTGOING &&
			direction != PageBackedGraphReadDirection::PBGRD_INCOMING) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency cursor has an unknown direction");
		}
		static_cast<void>(globalDirection(direction));
		std::unique_ptr<NativeReadSession> ownedSession;
		if (session == nullptr) {
			const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr);
			ownedSession.reset(new (memory) NativeReadSession(snapshot));
			session = ownedSession.get();
		}
		session->bind(snapshot);
		if (!session->node(nodeId).has_value()) {
			session->putNode(nodeId, entities_.readNode(snapshot, nodeId));
		}
		return PageBackedGraphAdjacencyCursor(
			*this, nodeId, direction,
				std::make_unique<PropertyCollectionRootScanCursor>(properties_.openScanCursor(snapshot)), session,
				std::move(ownedSession));
	}

	bool PageBackedGraphReadAdapter::visitAdjacentEdgesUntil(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
			size_t maximumRecords, const std::function<bool(const PageBackedGraphEdge &)> &visitor,
			NativeReadSession *session) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency traversal requires a nonzero record bound and visitor");
		}

		PageBackedGraphAdjacencyCursor cursor = openAdjacentEdgeCursor(snapshot, nodeId, direction, session);
		std::optional<PageBackedGraphAdjacencyContinuation> continuation;
		size_t remaining = maximumRecords;
		for (;;) {
			const size_t batchLimit = std::min(remaining, ADJACENCY_VISIT_BATCH_ENTRIES);
			const PageBackedGraphAdjacencyPage page = cursor.next(snapshot, continuation, batchLimit);
			for (const PageBackedGraphEdge &edge: page.edges) {
				if (!visitor(edge)) {
					return false;
				}
			}
			if (!page.continuation.has_value()) {
				return true;
			}
			if (page.edges.empty() || page.edges.size() > remaining) { // the internal cursor returns only bounded nonempty continuation pages.
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph adjacency cursor returned an invalid bounded page");
			}
			remaining -= page.edges.size();
			if (remaining == 0U) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph adjacency traversal exceeded its caller-supplied edge bound");
			}
			continuation = page.continuation;
		}
	}

	bool PageBackedGraphReadAdapter::visitAdjacentEdgeTopologiesUntil(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
			size_t maximumRecords, const std::function<bool(uint64_t, const EdgeEntityRecord &)> &visitor,
			NativeReadSession *session) const {
		if (nodeId == 0U || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max() || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency topology traversal requires a Node ID, bound, and visitor");
		}
		// Attribute topology-only work at the shared storage boundary.  This keeps
		// structural evidence independent of the query operator that requested it.
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.adjacency_topology_scan_calls", 1);
		}
		std::optional<NativeReadSession> localSession;
		NativeReadSession &readSession = selectNativeReadSession(snapshot, session, localSession);
		const GlobalAdjacencyDirection adjacencyDirection = globalDirection(direction);
		// Topology-only consumers need source-node liveness, not its labels or
		// Property collection. Resolve the Entity Map location directly so this
		// path never decodes a complete Node merely to validate an adjacency scan.
		if (!readSession.node(nodeId).has_value() &&
				!metadata_.findNodePageLocationOptional(snapshot, nodeId).has_value()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed graph adjacency topology references a missing source Node");
		}
		const GlobalAdjacencyRange edgeIds = adjacency_.scan(snapshot, adjacencyDirection, nodeId, maximumRecords);
		if (edgeIds.continuation.has_value()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency topology exceeded its caller-supplied edge bound");
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.adjacency_topology_scan_edges",
					static_cast<int64_t>(edgeIds.edge_ids.size()));
		}
		// Adjacency keys are ordered by Edge ID for one source Node. Resolve all
		// cache misses through one typed Entity-directory range cursor, then restore
		// the adjacency order while visiting. This keeps topology-only traversal
		// independent of the physical distance between sparse Edge IDs and avoids
		// scanning unrelated Entity pages.
		std::vector<uint64_t> missingEdgeIds;
		missingEdgeIds.reserve(edgeIds.edge_ids.size());
		for (const uint64_t edgeId : edgeIds.edge_ids) {
			if (!readSession.edge(edgeId).has_value()) {
				missingEdgeIds.push_back(edgeId);
			}
		}
		const std::vector<std::optional<EdgeEntityRecord>> missingRecords =
				entities_.readEdgesSorted(snapshot, missingEdgeIds);
		if (missingRecords.size() != missingEdgeIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency topology batch changed its cardinality");
		}
		for (const uint64_t edgeId : edgeIds.edge_ids) {
			std::optional<EdgeEntityRecord> cached = readSession.edge(edgeId);
			if (!cached.has_value()) {
				const auto position = std::ranges::lower_bound(missingEdgeIds, edgeId);
				if (position == missingEdgeIds.end() || *position != edgeId) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
											  "page-backed graph adjacency topology lost an Edge ID");
				}
				const size_t missingOrdinal = static_cast<size_t>(position - missingEdgeIds.begin());
				if (!missingRecords[missingOrdinal].has_value()) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
											  "page-backed graph adjacency topology references a missing Edge");
				}
				readSession.putEdge(edgeId, *missingRecords[missingOrdinal]);
				cached = missingRecords[missingOrdinal];
			}
			const EdgeEntityRecord &edge = *cached;
			const bool endpointMatches = direction == PageBackedGraphReadDirection::PBGRD_OUTGOING
																				 ? edge.source_node_id == nodeId
																				 : edge.target_node_id == nodeId;
			if (!endpointMatches) { // adjacency and Entity Map records share one pinned publication.
				throwAdapterError(FormatError::FERR_INVALID_STATE,
										  "page-backed graph adjacency topology has an inconsistent endpoint");
			}
			if (!visitor(edgeId, edge)) {
				return false;
			}
		}
		return true;
	}

	bool PageBackedGraphReadAdapter::visitAdjacentEdgeRefsForNodesUntil(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const PageBackedGraphReadDirection direction, const size_t maximumRecords,
			const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session,
			const std::function<void(size_t)> &retainedBufferAdmission) const {
		if (nodeIds.empty() || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max() || !visitor ||
				!std::ranges::is_sorted(nodeIds) || std::ranges::adjacent_find(nodeIds) != nodeIds.end()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed batch adjacency traversal requires sorted nodes, a bound, and a visitor");
		}
		const GlobalAdjacencyDirection globalDirectionValue = globalDirection(direction);
		const size_t scanLimit = maximumRecords + 1U;
		std::optional<NativeReadSession> localSession;
		NativeReadSession &active = selectNativeReadSession(snapshot, session, localSession);
		size_t cursorBoundaryBytes = 0U;
		const auto admit = [&](size_t base, size_t count, size_t width) {
			if (width != 0U && count > (std::numeric_limits<size_t>::max() - base) / width) {
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "batch adjacency admission size overflowed");
			}
			const size_t bytes = base + count * width;
			if (bytes > std::numeric_limits<size_t>::max() - cursorBoundaryBytes)
				throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW, "batch adjacency cursor admission overflowed");
			if (retainedBufferAdmission) retainedBufferAdmission(bytes + cursorBoundaryBytes);
			return bytes;
		};
		std::vector<std::pair<uint64_t, uint64_t>> keys;
		const size_t keyLimit = std::min(maximumRecords, MULTI_NODE_ADJACENCY_BATCH_ENTRIES);
		size_t keyBytes = 0U;
		bool stoppedByVisitor = false;
		bool boundExceeded = false;
		size_t matchedRecords = 0U;
		const auto flush = [&]() {
			if (keys.empty()) {
				return true;
			}
			std::vector<uint64_t> edgeIds;
			admit(keyBytes, keys.size(), sizeof(uint64_t));
			edgeIds.reserve(keys.size());
			const size_t gatherBytes = admit(keyBytes, edgeIds.capacity(), sizeof(uint64_t));
			for (const auto &[nodeId, edgeId]: keys) {
				static_cast<void>(nodeId);
				edgeIds.push_back(edgeId);
			}
			size_t comparisons = 0U;
			std::sort(edgeIds.begin(), edgeIds.end(), [&](uint64_t left, uint64_t right) {
				if ((comparisons++ & 255U) == 0U) admit(gatherBytes, 0U, 0U);
				return left < right;
			});
			if (const auto duplicate = std::ranges::adjacent_find(edgeIds); duplicate != edgeIds.end()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
						"page-backed batch adjacency traversal returned a duplicate Edge");
			}
			// Capture cache hits before any session mutation can evict them. Keep the
			// complete ordinal matrix alive through callbacks and gather only misses.
			admit(gatherBytes, edgeIds.size(), sizeof(std::optional<EdgeEntityRecord>));
			std::vector<std::optional<EdgeEntityRecord>> records(edgeIds.size());
			const size_t recordBytes = admit(gatherBytes, records.capacity(), sizeof(records.front()));
			admit(recordBytes, edgeIds.size(), sizeof(uint64_t));
			std::vector<uint64_t> missingIds;
			missingIds.reserve(edgeIds.size());
			for (size_t i = 0; i < edgeIds.size(); ++i) {
				records[i] = active.edge(edgeIds[i]);
				if (!records[i]) missingIds.push_back(edgeIds[i]);
			}
			const size_t baseBytes = admit(recordBytes, missingIds.capacity(), sizeof(uint64_t));
			if (!missingIds.empty()) {
				admit(baseBytes, missingIds.size(), sizeof(std::optional<EdgeEntityRecord>));
				const auto missingRecords = entities_.readEdgesSorted(snapshot, missingIds,
					[&](size_t bytes) { admit(baseBytes, bytes, 1U); });
				admit(baseBytes, missingRecords.capacity(), sizeof(std::optional<EdgeEntityRecord>));
				if (missingRecords.size() != missingIds.size()) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
						"page-backed batch adjacency traversal changed Edge cardinality");
				}
				size_t missing = 0U;
				for (auto &record : records) if (!record) record = missingRecords[missing++];
			}
			const size_t workingBytes = baseBytes;
			for (const auto &[nodeId, edgeId]: keys) {
				admit(workingBytes, 0U, 0U);
				const auto position = std::ranges::lower_bound(edgeIds, edgeId);
				if (position == edgeIds.end() || *position != edgeId) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
							"page-backed batch adjacency traversal lost an Edge identity");
				}
				const size_t ordinal = static_cast<size_t>(position - edgeIds.begin());
				if (!records[ordinal].has_value()) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
							"page-backed batch adjacency traversal lost an Edge record");
				}
				const EdgeEntityRecord &record = *records[ordinal];
				const bool endpointMatches = direction == PageBackedGraphReadDirection::PBGRD_OUTGOING
						? record.source_node_id == nodeId : record.target_node_id == nodeId;
				if (!endpointMatches || record.source_node_id == 0U || record.target_node_id == 0U) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
							"page-backed batch adjacency traversal returned an inconsistent endpoint");
				}
            }
            // Authenticate the whole private batch before any consumer can
            // apply a type predicate, stop early or produce side effects. A bad
            // later record must not be hidden by an earlier visitor return.
            for (const auto &[nodeId, edgeId]: keys) {
                static_cast<void>(nodeId);
                admit(workingBytes, 0U, 0U);
                const size_t ordinal = static_cast<size_t>(std::ranges::lower_bound(edgeIds, edgeId) - edgeIds.begin());
                const EdgeEntityRecord &record = *records[ordinal];
				active.putEdge(edgeId, record);
				if (!visitor({edgeId, true, record.source_node_id, record.target_node_id, record.type_id})) {
					stoppedByVisitor = true;
					keys.clear();
					return false;
				}
			}
			keys.clear();
			return true;
		};
		const bool exhausted = adjacency_.visitForNodes(snapshot, globalDirectionValue, nodeIds, scanLimit,
				[&](const uint64_t nodeId, const uint64_t edgeId) {
					if (matchedRecords == maximumRecords) {
						boundExceeded = true;
						return false;
					}
					++matchedRecords;
					admit(keyBytes, 0U, 0U);
					if (keys.size() == keys.capacity()) {
						const size_t next = keys.capacity() == 0U ? 1U :
							keys.capacity() + std::min(keys.capacity(), keyLimit - keys.capacity());
						bool grow = true;
						try {
							// The old key buffer is gone before gather/record allocation.
							// Admit that future frame separately from the reserve overlap.
							admit(0U, next, sizeof(keys.front()) + 2U * sizeof(uint64_t) +
								2U * sizeof(std::optional<EdgeEntityRecord>) + sizeof(std::optional<EntityPageLocation>) + sizeof(size_t));
						} catch (const LogicalGraphBufferPressure &) {
							if (keys.empty()) throw;
							if (!flush()) return false;
							admit(keyBytes, 0U, 0U);
							grow = false;
						}
						if (grow) {
							admit(keyBytes, next, sizeof(keys.front()));
							keys.reserve(next);
							keyBytes = admit(0U, keys.capacity(), sizeof(keys.front()));
						}
					}
					keys.emplace_back(nodeId, edgeId);
					return keys.size() != keyLimit || flush();
				}, [&](size_t bytes) {
					cursorBoundaryBytes = bytes;
					admit(keyBytes, 0U, 0U);
				});
		// The range cursor has been destroyed; remaining keys survive the final flush.
		cursorBoundaryBytes = 0U;
		admit(keyBytes, 0U, 0U);
		if (stoppedByVisitor) {
			std::vector<std::pair<uint64_t, uint64_t>>{}.swap(keys);
			admit(0U, 0U, 0U);
			return false;
		}
		if (boundExceeded || !exhausted) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed batch adjacency traversal exceeded its Edge bound");
		}
		const bool result = flush();
		std::vector<std::pair<uint64_t, uint64_t>>{}.swap(keys);
		admit(0U, 0U, 0U);
		return result;
	}

} // namespace graph::storage
