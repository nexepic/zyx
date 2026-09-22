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
		constexpr size_t TOPOLOGY_ENTITY_SCAN_THRESHOLD = 512U;

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
											   PropertyCollectionRootScanCursor *scanCursor) const {
		std::optional<PropertyCollectionRoot> root = scanCursor == nullptr
															 ? properties_.readEntityOptional(snapshot, key)
															 : scanCursor->readEntityOptional(snapshot, key);
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
												PropertyCollectionRootScanCursor *scanCursor) const {
		return {edgeId, record.source_node_id, record.target_node_id, record.type_id,
				readProperties(snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId}, scanCursor)};
	}

	PageBackedGraphNode PageBackedGraphReadAdapter::readNode(const MetadataSnapshotPin &snapshot,
															 uint64_t nodeId, NativeReadSession *session) const {
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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

	std::vector<PropertyCollectionRootEntry> PageBackedGraphReadAdapter::readNodeProperties(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, NativeReadSession *session) const {
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
		static_cast<void>(readNodeRecord(snapshot, nodeId, &active));
		return readProperties(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId},
																										 &active.propertyCursor(properties_, snapshot));
	}

	std::vector<std::vector<PropertyCollectionRootEntry>> PageBackedGraphReadAdapter::readNodePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			NativeReadSession *session) const {
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
		std::vector<std::vector<PropertyCollectionRootEntry>> result(nodeIds.size());
		uint64_t previousId = 0U;
		PropertyCollectionRootScanCursor &cursor = active.propertyCursor(properties_, snapshot);
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			const uint64_t nodeId = nodeIds[index];
			if (nodeId == 0U || nodeId <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
						"page-backed sorted Node Property root read requires strictly increasing Node IDs");
			}
			previousId = nodeId;
			if (const auto root = cursor.readEntityOptional(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId});
				root.has_value()) {
				result[index] = root->entries;
			}
		}
		return result;
	}

	std::vector<uint64_t> PageBackedGraphReadAdapter::readNodeLabelTokens(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, NativeReadSession *session) const {
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
																		 const uint32_t propertyToken, NativeReadSession *session) const {
		if (nodeId == 0U || propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
								"page-backed Node Property presence read requires nonzero identifiers");
		}
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
		if (const auto cachedValue = activeSession.property(false, nodeId, propertyToken); cachedValue.has_value()) {
			const bool present = cachedValue->has_value();
			activeSession.putPropertyPresence(false, nodeId, propertyToken, present);
			return present;
		}
		if (const auto cached = activeSession.propertyPresence(false, nodeId, propertyToken); cached.has_value()) {
			return *cached;
		}
		const bool present = activeSession.propertyCursor(properties_, snapshot).readEntityPropertyExists(
				snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}, propertyToken);
		activeSession.putPropertyPresence(false, nodeId, propertyToken, present);
		return present;
	}

	std::vector<std::optional<PropertyValue>> PageBackedGraphReadAdapter::readNodePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const uint32_t propertyToken, NativeReadSession *session) const {
		if (propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed sorted Node Property read requires a nonzero Property token");
		}
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
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
			const std::span<const uint32_t> propertyTokens, NativeReadSession *session) const {
		if (propertyTokens.empty()) {
			return {};
		}
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
		if (nodeIds.size() > std::numeric_limits<size_t>::max() / propertyTokens.size()) {
			throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW,
							  "page-backed Node Property column read result size overflows");
		}
		std::vector<std::vector<std::optional<PropertyValue>>> columns(
				propertyTokens.size(), std::vector<std::optional<PropertyValue>>(nodeIds.size()));
		std::vector<uint64_t> missingIds;
		std::vector<size_t> missingOrdinals;
		missingIds.reserve(nodeIds.size());
		missingOrdinals.reserve(nodeIds.size());
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
			bool missing = false;
			for (size_t tokenIndex = 0U; tokenIndex < propertyTokens.size(); ++tokenIndex) {
				const uint32_t token = propertyTokens[tokenIndex];
				if (const auto cached = active.property(false, nodeIds[owner], token); cached.has_value()) {
					columns[tokenIndex][owner] = *cached;
				} else if (const auto present = active.propertyPresence(false, nodeIds[owner], token);
						present.has_value() && !*present) {
					columns[tokenIndex][owner] = std::nullopt;
				} else {
					missing = true;
				}
			}
			if (missing) {
				missingIds.push_back(nodeIds[owner]);
				missingOrdinals.push_back(owner);
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
					snapshot, missingKeys, propertyTokens, entries);
			for (size_t owner = 0U; owner < missingIds.size(); ++owner) {
				const size_t outputOwner = missingOrdinals[owner];
				for (size_t token = 0U; token < propertyTokens.size(); ++token) {
					const auto &entry = entries[owner * propertyTokens.size() + token];
					const std::optional<PropertyValue> value = entry.has_value()
							? std::optional<PropertyValue>{entry->value} : std::nullopt;
					columns[token][outputOwner] = value;
					active.putProperty(false, missingIds[owner], propertyTokens[token], value);
				}
			}
		}
		for (size_t owner = 0U; owner < nodeIds.size(); ++owner) {
			for (size_t token = 0U; token < propertyTokens.size(); ++token) {
				if (const auto cached = active.property(false, nodeIds[owner], propertyTokens[token]); cached.has_value()) {
					columns[token][owner] = *cached;
				}
			}
		}
		return columns;
	}

	std::pair<std::vector<std::vector<std::optional<PropertyValue>>>,
				std::vector<std::optional<size_t>>>
	PageBackedGraphReadAdapter::readNodePropertyColumnsAndCountsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const std::span<const uint32_t> propertyTokens, NativeReadSession *session) const {
		if (propertyTokens.empty()) {
			return {{}, std::vector<std::optional<size_t>>(nodeIds.size(), size_t{0U})};
		}
		if (nodeIds.size() > std::numeric_limits<size_t>::max() / propertyTokens.size()) {
			throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW,
							  "combined Node Property read result size overflows");
		}
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
		std::vector<EntityPropertyKey> keys;
		keys.reserve(nodeIds.size());
		uint64_t previousId = 0U;
		for (const uint64_t nodeId: nodeIds) {
			if (nodeId == 0U || nodeId <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "combined Node Property read requires strictly increasing Node IDs");
			}
			previousId = nodeId;
			keys.push_back({EntityPropertyOwnerKind::EPOK_NODE, nodeId});
		}
		std::vector<std::optional<PropertyCollectionRootEntry>> entries(
				 nodeIds.size() * propertyTokens.size());
		std::vector<std::optional<size_t>> counts(nodeIds.size());
		std::vector<std::vector<std::optional<PropertyValue>>> columns(
				propertyTokens.size(), std::vector<std::optional<PropertyValue>>(nodeIds.size()));
		std::vector<size_t> missingOwners;
		std::vector<size_t> countOnlyOwners;
		missingOwners.reserve(nodeIds.size());
		countOnlyOwners.reserve(nodeIds.size());
		for (size_t owner = 0U; owner < nodeIds.size(); ++owner) {
			bool missingValue = false;
			for (size_t token = 0U; token < propertyTokens.size(); ++token) {
				const auto cached = active.property(false, nodeIds[owner], propertyTokens[token]);
				if (cached.has_value()) {
					columns[token][owner] = *cached;
				} else {
					missingValue = true;
				}
			}
			if (missingValue) {
				missingOwners.push_back(owner);
			} else {
				countOnlyOwners.push_back(owner);
			}
		}
		if (!missingOwners.empty()) {
			std::vector<EntityPropertyKey> missingKeys;
			missingKeys.reserve(missingOwners.size());
			std::vector<std::optional<PropertyCollectionRootEntry>> missingEntries(
					missingOwners.size() * propertyTokens.size());
			std::vector<std::optional<size_t>> missingCounts(missingOwners.size());
			for (const size_t owner: missingOwners) {
				missingKeys.push_back(keys[owner]);
			}
			// Keep the query-owned Property cursor alive across adjacent candidate
			// batches. This avoids restarting the Property Directory root descent for
			// every range batch while preserving the same snapshot and validation fence.
			active.propertyCursor(properties_, snapshot).readEntityPropertiesAndCountsOptionalSorted(
					snapshot, missingKeys, propertyTokens, missingEntries, missingCounts);
			for (size_t index = 0U; index < missingOwners.size(); ++index) {
				const size_t owner = missingOwners[index];
				counts[owner] = missingCounts[index];
				for (size_t token = 0U; token < propertyTokens.size(); ++token) {
					const auto &entry = missingEntries[index * propertyTokens.size() + token];
					const std::optional<PropertyValue> value =
							entry.has_value() ? std::optional<PropertyValue>{entry->value} : std::nullopt;
					columns[token][owner] = value;
					active.putProperty(false, nodeIds[owner], propertyTokens[token], value);
				}
			}
		}
		if (!countOnlyOwners.empty()) {
			std::vector<EntityPropertyKey> countKeys;
			std::vector<std::optional<size_t>> cachedCounts;
			countKeys.reserve(countOnlyOwners.size());
			cachedCounts.resize(countOnlyOwners.size());
			for (const size_t owner: countOnlyOwners) {
				countKeys.push_back(keys[owner]);
			}
			active.propertyCursor(properties_, snapshot).readEntityPropertyCountsSorted(snapshot, countKeys, cachedCounts);
			for (size_t index = 0U; index < countOnlyOwners.size(); ++index) {
				counts[countOnlyOwners[index]] = cachedCounts[index];
			}
		}
		return {std::move(columns), std::move(counts)};
	}

	size_t PageBackedGraphReadAdapter::nodePropertyCount(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, NativeReadSession *session) const {
		if (nodeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed Node Property count requires a nonzero Node ID");
		}
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, nodeId};
		return properties_.readEntityPropertyCount(snapshot, key).value_or(0U);
	}

	std::vector<std::optional<size_t>> PageBackedGraphReadAdapter::nodePropertyCountsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			NativeReadSession *session) const {
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		static_cast<void>(readNodeRecord(snapshot, nodeId, &activeSession));
		if (labelTokens.empty()) {
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
			const std::span<const uint64_t> labelTokens, NativeReadSession *session) const {
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
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
		std::vector<bool> result(nodeIds.size(), false);
		std::vector<uint64_t> missingIds;
		missingIds.reserve(nodeIds.size());
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			// Label-only cache entries are not sufficient here: the batch result
			// must retain an authenticated entity record for every requested node.
			if (!active.node(nodeIds[index]).has_value()) {
				missingIds.push_back(nodeIds[index]);
			}
		}
		const std::vector<std::optional<NodeEntityRecord>> records =
				missingIds.empty() ? std::vector<std::optional<NodeEntityRecord>>{} : entities_.readNodesSorted(snapshot, missingIds);
		if (records.size() != missingIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed sorted Node label read changed record cardinality");
		}
		// Keep label evaluation independent from the bounded session cache. A
		// caller may submit more IDs than the cache can retain, and falling back to
		// one scalar Entity lookup per evicted ID defeats the sorted batch contract.
		std::vector<NodeEntityRecord> resolved(nodeIds.size());
		std::vector<bool> resolvedFlags(nodeIds.size(), false);
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
		std::vector<std::vector<uint64_t>> decodedLabels(nodeIds.size());
		std::vector<bool> hasLabels(nodeIds.size(), false);
		if (!labelTokens.empty()) {
			std::vector<NodeEntityRecord> uncachedRecords;
			std::vector<size_t> uncachedOrdinals;
			uncachedRecords.reserve(nodeIds.size());
			uncachedOrdinals.reserve(nodeIds.size());
			for (size_t index = 0U; index < nodeIds.size(); ++index) {
				if (const auto cachedLabels = active.nodeLabels(nodeIds[index]); cachedLabels.has_value()) {
					decodedLabels[index] = *cachedLabels;
					hasLabels[index] = true;
				} else {
					uncachedRecords.push_back(resolved[index]);
					uncachedOrdinals.push_back(index);
				}
			}
			if (!uncachedRecords.empty()) {
				const std::vector<std::vector<uint64_t>> batchLabels =
						labels_.readNodeLabelsSorted(snapshot, uncachedRecords);
				if (batchLabels.size() != uncachedOrdinals.size()) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
							"page-backed sorted Node label read changed decoded batch cardinality");
				}
				for (size_t index = 0U; index < uncachedOrdinals.size(); ++index) {
					const size_t ordinal = uncachedOrdinals[index];
					decodedLabels[ordinal] = batchLabels[index];
					hasLabels[ordinal] = true;
					active.putNodeLabels(nodeIds[ordinal], decodedLabels[ordinal]);
				}
			}
		}
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			bool matches = true;
			if (!labelTokens.empty()) {
				if (!hasLabels[index]) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
							"page-backed sorted Node label read lost decoded labels");
				}
				for (const uint64_t token: labelTokens) {
					if (!std::ranges::binary_search(decodedLabels[index], token)) {
						matches = false;
						break;
					}
				}
			}
			result[index] = matches;
		}
		return result;
	}

	std::optional<PropertyValue> PageBackedGraphReadAdapter::readEdgeProperty(
			const MetadataSnapshotPin &snapshot, const uint64_t edgeId, const uint32_t propertyToken,
			NativeReadSession *session) const {
		if (edgeId == 0U || propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed Edge Property read requires nonzero identifiers");
		}
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
																		 const uint32_t propertyToken, NativeReadSession *session) const {
		if (edgeId == 0U || propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
								"page-backed Edge Property presence read requires nonzero identifiers");
		}
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
		if (const auto cachedValue = activeSession.property(true, edgeId, propertyToken); cachedValue.has_value()) {
			const bool present = cachedValue->has_value();
			activeSession.putPropertyPresence(true, edgeId, propertyToken, present);
			return present;
		}
		if (const auto cached = activeSession.propertyPresence(true, edgeId, propertyToken); cached.has_value()) {
			return *cached;
		}
		const bool present = activeSession.propertyCursor(properties_, snapshot).readEntityPropertyExists(
				snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId}, propertyToken);
		activeSession.putPropertyPresence(true, edgeId, propertyToken, present);
		return present;
	}

	std::vector<std::optional<PropertyValue>> PageBackedGraphReadAdapter::readEdgePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			const uint32_t propertyToken, NativeReadSession *session) const {
		if (propertyToken == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed sorted Edge Property read requires a nonzero Property token");
		}
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
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
			const std::span<const uint32_t> propertyTokens, NativeReadSession *session) const {
		if (propertyTokens.empty()) {
			return {};
		}
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
		if (edgeIds.size() > std::numeric_limits<size_t>::max() / propertyTokens.size()) {
			throwAdapterError(FormatError::FERR_ARITHMETIC_OVERFLOW,
							  "page-backed Edge Property column read result size overflows");
		}
		std::vector<std::vector<std::optional<PropertyValue>>> columns(
			propertyTokens.size(), std::vector<std::optional<PropertyValue>>(edgeIds.size()));
		uint64_t previousId = 0U;
		uint32_t previousToken = 0U;
		for (const uint32_t token: propertyTokens) {
			if (token == 0U || token <= previousToken) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed Edge Property columns require strictly increasing nonzero tokens");
			}
			previousToken = token;
		}
		std::vector<uint64_t> missingIds;
		std::vector<size_t> missingOrdinals;
		for (size_t owner = 0U; owner < edgeIds.size(); ++owner) {
			if (edgeIds[owner] == 0U || edgeIds[owner] <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed Edge Property columns require strictly increasing Edge IDs");
			}
			previousId = edgeIds[owner];
			bool missing = false;
			for (size_t tokenIndex = 0U; tokenIndex < propertyTokens.size(); ++tokenIndex) {
				const uint32_t token = propertyTokens[tokenIndex];
				if (const auto cached = active.property(true, edgeIds[owner], token); cached.has_value()) {
					columns[tokenIndex][owner] = *cached;
				} else if (const auto present = active.propertyPresence(true, edgeIds[owner], token);
						present.has_value() && !*present) {
					columns[tokenIndex][owner] = std::nullopt;
				} else {
					missing = true;
				}
			}
			if (missing) {
				missingIds.push_back(edgeIds[owner]);
				missingOrdinals.push_back(owner);
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
					snapshot, missingKeys, propertyTokens, entries);
			for (size_t owner = 0U; owner < missingIds.size(); ++owner) {
				const size_t outputOwner = missingOrdinals[owner];
				for (size_t token = 0U; token < propertyTokens.size(); ++token) {
					const auto &entry = entries[owner * propertyTokens.size() + token];
					const std::optional<PropertyValue> value = entry.has_value()
							? std::optional<PropertyValue>{entry->value} : std::nullopt;
					columns[token][outputOwner] = value;
					active.putProperty(true, missingIds[owner], propertyTokens[token], value);
				}
			}
		}
		for (size_t owner = 0U; owner < edgeIds.size(); ++owner) {
			for (size_t token = 0U; token < propertyTokens.size(); ++token) {
				if (const auto cached = active.property(true, edgeIds[owner], propertyTokens[token]); cached.has_value()) {
					columns[token][owner] = *cached;
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
			const size_t maximumPropertyEntries, NativeReadSession *session) const {
		if (nodeIds.empty() || labelToken == 0U || maximumPropertyEntries == 0U ||
			propertyTokens.empty() || propertyTokens.size() != expectedValues.size() ||
			propertyTokens.size() > maximumPropertyEntries ||
			nodeIds.size() > std::numeric_limits<size_t>::max() / propertyTokens.size()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed exact Node batch verification has invalid identifiers or Property bounds");
		}
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		std::vector<size_t> predicateOrdinals(propertyTokens.size());
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
		std::vector<PropertyValue> sortedExpected;
		sortedTokens.reserve(propertyTokens.size());
		sortedExpected.reserve(expectedValues.size());
		for (const size_t ordinal: predicateOrdinals) {
			if (!sortedTokens.empty() && sortedTokens.back() == propertyTokens[ordinal]) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
								  "page-backed exact Node batch verification has duplicate Property tokens");
			}
			sortedTokens.push_back(propertyTokens[ordinal]);
			sortedExpected.push_back(expectedValues[ordinal]);
		}

		std::vector<EntityPropertyKey> keys;
		keys.reserve(nodeIds.size());
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
		const std::vector<std::optional<NodeEntityRecord>> records = entities_.readNodesSorted(snapshot, nodeIds);
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
			const std::vector<uint64_t> labels = labels_.readNodeLabels(snapshot, record);
			if (std::ranges::find(labels, labelToken) == labels.end()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed exact Node batch verification found a candidate outside the indexed label");
			}
		}
		// Count and selected-value validation share the same owner-keyed directory
		// location. Resolve both in one merge so digest-collision candidates do not
		// reopen the Property Directory or page route for the same root.
		std::vector<std::optional<size_t>> propertyCounts(keys.size());
		std::vector<std::optional<PropertyCollectionRootEntry>> selected(keys.size() * sortedTokens.size());
		properties_.readEntityPropertiesAndCountsOptionalSorted(snapshot, keys, sortedTokens, selected, propertyCounts);
		for (const std::optional<size_t> count: propertyCounts) {
			if (!count.has_value() || *count > maximumPropertyEntries) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed exact Node batch verification exceeded the Property entry bound");
			}
		}
		for (size_t owner = 0U; owner < keys.size(); ++owner) {
			for (size_t token = 0U; token < sortedTokens.size(); ++token) {
				const auto &entry = selected[owner * sortedTokens.size() + token];
				if (!entry.has_value() || entry->value != sortedExpected[token]) {
					throwAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed exact Node batch verification found a stale indexed Property");
				}
			}
		}
	}

	PageBackedGraphEdge PageBackedGraphReadAdapter::readEdge(const MetadataSnapshotPin &snapshot,
															 uint64_t edgeId, NativeReadSession *session) const {
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
			const MetadataSnapshotPin &snapshot, const uint64_t edgeId, NativeReadSession *session) const {
		if (edgeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed Edge Property root read requires a nonzero logical Edge ID");
		}
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
		return readProperties(snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId},
																										 &active.propertyCursor(properties_, snapshot));
	}

	std::vector<std::vector<PropertyCollectionRootEntry>> PageBackedGraphReadAdapter::readEdgePropertiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			NativeReadSession *session) const {
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
		std::vector<std::vector<PropertyCollectionRootEntry>> result(edgeIds.size());
		uint64_t previousId = 0U;
		PropertyCollectionRootScanCursor &cursor = active.propertyCursor(properties_, snapshot);
		for (size_t index = 0U; index < edgeIds.size(); ++index) {
			const uint64_t edgeId = edgeIds[index];
			if (edgeId == 0U || edgeId <= previousId) {
				throwAdapterError(FormatError::FERR_INVALID_HEADER,
						"page-backed sorted Edge Property root read requires strictly increasing Edge IDs");
			}
			previousId = edgeId;
			if (const auto root = cursor.readEntityOptional(snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId});
				root.has_value()) {
				result[index] = root->entries;
			}
		}
		return result;
	}

	std::vector<EdgeEntityRecord> PageBackedGraphReadAdapter::readEdgeTopologiesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			NativeReadSession *session) const {
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
		return entities_.visitNodesUntil(
				snapshot, maximumRecords,
				[this, &snapshot, labelTokens, &visitor, &activeSession](const uint64_t nodeId, const NodeEntityRecord &record) {
					activeSession.putNode(nodeId, record);
					bool labelsMatch = true;
					if (!labelTokens.empty()) {
						const std::vector<uint64_t> actual = [&]() {
							if (const auto cached = activeSession.nodeLabels(nodeId); cached.has_value()) {
								return *cached;
							}
							const auto inlineEnd = record.inline_label_ids.begin() + record.inline_label_count;
							bool allInline = true;
							for (const uint64_t token: labelTokens) {
								if (!std::ranges::binary_search(record.inline_label_ids.begin(), inlineEnd, token)) {
									allInline = false;
									break;
								}
							}
			if (record.label_overflow.isEmpty()) {
				std::vector<uint64_t> inlineLabels(record.inline_label_ids.begin(), inlineEnd);
				activeSession.putNodeLabels(nodeId, inlineLabels);
				return inlineLabels;
			}
			if (allInline) {
				return std::vector<uint64_t>(record.inline_label_ids.begin(), inlineEnd);
			}
							const std::vector<uint64_t> labels = labels_.readNodeLabels(snapshot, record);
							activeSession.putNodeLabels(nodeId, labels);
							return labels;
						}();
						for (const uint64_t token: labelTokens) {
							if (!std::ranges::binary_search(actual, token)) {
								labelsMatch = false;
								break;
							}
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		if (maximumRecords == 0U || maximumMetadataPages == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph resumable Node scan requires positive bounds");
		}
		PageBackedGraphNodePage result;
		size_t metadataPagesVisited = 0U;
		NativeReadSession localSession;
		NativeReadSession &activeSession = session == nullptr ? localSession : *session;
		activeSession.bind(snapshot);
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
		const std::vector<std::vector<uint64_t>> labelSets =
				labels_.readNodeLabelsSorted(snapshot, records);
		if (labelSets.size() != nodeIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed Node scan changed its label cardinality");
		}
		const std::vector<std::vector<PropertyCollectionRootEntry>> propertySets =
				readNodePropertiesSorted(snapshot, nodeIds, &activeSession);
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
			NativeReadSession *session) const {
		return scanAdjacentEdges(snapshot, nodeId, direction, continuation, maximumRecords, nullptr, session);
	}

	PageBackedGraphAdjacencyPage PageBackedGraphReadAdapter::scanAdjacentEdges(
		const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
		std::optional<PageBackedGraphAdjacencyContinuation> continuation, size_t maximumRecords,
		PropertyCollectionRootScanCursor *propertyCursor, NativeReadSession *session) const {
		if (nodeId == 0U || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency scan requires a Node ID and positive record bound");
		}
		NativeReadSession localSession;
		NativeReadSession &readSession = session == nullptr ? localSession : *session;
		readSession.bind(snapshot);
		const GlobalAdjacencyDirection adjacencyDirection = globalDirection(direction);
		if (continuation.has_value() && (continuation->previous_edge_id == 0U || continuation->next_edge_id == 0U)) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency continuation has a zero Edge ID");
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
									 continuation->previous_edge_id};
		}
		const size_t boundedRecords = std::min(maximumRecords, maximum_adjacency_edges_);
		if (boundedRecords == 0U) { // constructor validation keeps both bounds positive.
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency scan has an invalid configured edge bound");
		}
		const GlobalAdjacencyRange edgeIds = adjacency_.scan(snapshot, adjacencyDirection, nodeId, boundedRecords + 1U,
																									 adjacencyContinuation);
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
		result.edges.reserve(resultCount);
		// Adjacency IDs are returned in logical order. Resolve only cache misses in
		// one sorted Entity-Map/page-local batch, then restore that order while
		// materializing the public page. This removes one Directory descent per Edge
		// without exposing physical locations or changing duplicate/order semantics.
		std::vector<uint64_t> missingEdgeIds;
		missingEdgeIds.reserve(resultCount);
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			const uint64_t edgeId = edgeIds.edge_ids[ordinal];
			if (!readSession.edge(edgeId).has_value()) {
				missingEdgeIds.push_back(edgeId);
			}
		}
		std::ranges::sort(missingEdgeIds);
		missingEdgeIds.erase(std::unique(missingEdgeIds.begin(), missingEdgeIds.end()), missingEdgeIds.end());
		const std::vector<std::optional<EdgeEntityRecord>> missingRecords =
				entities_.readEdgesSorted(snapshot, missingEdgeIds);
		if (missingRecords.size() != missingEdgeIds.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency batch changed Edge record cardinality");
		}
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			const uint64_t edgeId = edgeIds.edge_ids[ordinal];
			std::optional<EdgeEntityRecord> cached = readSession.edge(edgeId);
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
			result.edges.push_back(materializeEdge(snapshot, edgeId, edge, propertyCursor));
		}
		if (edgeIds.edge_ids.size() > resultCount) {
			if (result.edges.empty()) { // a continuation requires a positive bounded result count.
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed adjacency scan cannot return a continuation without an Edge");
			}
			result.continuation = PageBackedGraphAdjacencyContinuation{result.edges.back().edge_id,
																												   edgeIds.edge_ids[resultCount]};
		}
		return result;
	}

	PageBackedGraphAdjacencyTopologyPage PageBackedGraphReadAdapter::scanAdjacentEdgeTopologies(
			const MetadataSnapshotPin &snapshot, const uint64_t nodeId, const PageBackedGraphReadDirection direction,
			const std::optional<PageBackedGraphAdjacencyContinuation> continuation,
			const size_t maximumRecords, NativeReadSession *session) const {
		if (nodeId == 0U || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency topology scan requires a Node ID and positive record bound");
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.adjacency_topology_scan_calls", 1);
		}
		NativeReadSession localSession;
		NativeReadSession &readSession = session == nullptr ? localSession : *session;
		readSession.bind(snapshot);
		const GlobalAdjacencyDirection adjacencyDirection = globalDirection(direction);
		if (continuation.has_value() &&
			(continuation->previous_edge_id == 0U || continuation->next_edge_id == 0U)) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency topology continuation has a zero Edge ID");
		}
		if (!readSession.node(nodeId).has_value()) {
			readSession.putNode(nodeId, entities_.readNode(snapshot, nodeId));
		}
		std::optional<GlobalAdjacencyContinuation> adjacencyContinuation;
		if (continuation.has_value()) {
			adjacencyContinuation = {snapshot.superblock().generation, adjacencyDirection, nodeId,
									 continuation->previous_edge_id};
		}
		const size_t boundedRecords = std::min(maximumRecords, maximum_adjacency_edges_);
		if (boundedRecords == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency topology scan has an invalid configured edge bound");
		}
		const GlobalAdjacencyRange edgeIds =
				adjacency_.scan(snapshot, adjacencyDirection, nodeId, boundedRecords + 1U, adjacencyContinuation);
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
		result.edge_ids.reserve(resultCount);
		result.edges.reserve(resultCount);
		const std::span<const uint64_t> requestedEdgeIds(edgeIds.edge_ids.data(), resultCount);
		std::vector<uint64_t> missingEdgeIds;
		std::vector<size_t> missingOrdinals;
		missingEdgeIds.reserve(resultCount);
		missingOrdinals.reserve(resultCount);
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			if (!readSession.edge(requestedEdgeIds[ordinal]).has_value()) {
				missingEdgeIds.push_back(requestedEdgeIds[ordinal]);
				missingOrdinals.push_back(ordinal);
			}
		}
		const std::vector<std::optional<EdgeEntityRecord>> missingRecords =
				entities_.readEdgesSorted(snapshot, missingEdgeIds);
		if (missingRecords.size() != missingOrdinals.size()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
					"page-backed graph adjacency topology batch changed its cardinality");
		}
		std::vector<std::optional<EdgeEntityRecord>> records(resultCount);
		std::vector<bool> wasMissing(resultCount, false);
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			records[ordinal] = readSession.edge(requestedEdgeIds[ordinal]);
			wasMissing[ordinal] = !records[ordinal].has_value();
		}
		for (size_t index = 0U; index < missingOrdinals.size(); ++index) {
			records[missingOrdinals[index]] = missingRecords[index];
		}
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			const uint64_t edgeId = requestedEdgeIds[ordinal];
			if (!records[ordinal].has_value()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed graph adjacency topology references a missing Edge");
			}
			const EdgeEntityRecord edge = *records[ordinal];
			if (wasMissing[ordinal]) {
				readSession.putEdge(edgeId, edge);
			}
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
			ownedSession = std::make_unique<NativeReadSession>();
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
		NativeReadSession localSession;
		NativeReadSession &readSession = session == nullptr ? localSession : *session;
		readSession.bind(snapshot);
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
		if (edgeIds.edge_ids.size() <= TOPOLOGY_ENTITY_SCAN_THRESHOLD) {
			// Keep strict early termination: the callback may stop before the next
			// logical record, so this small path intentionally resolves one Edge at a
			// time. Large bounded scans use the sequential Entity Map pass below.
			for (const uint64_t edgeId: edgeIds.edge_ids) {
				const std::optional<EdgeEntityRecord> cached = readSession.edge(edgeId);
				const EdgeEntityRecord edge = cached.has_value() ? *cached : entities_.readEdge(snapshot, edgeId);
				if (!cached.has_value()) {
					readSession.putEdge(edgeId, edge);
				}
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

		// Large topology-only counts are sorted by Edge ID in both directories. A
		// single sequential Entity Map pass avoids one metadata lookup per Edge,
		// while the adjacency result remains bounded by the caller.
		size_t requested = 0U;
		bool stoppedByVisitor = false;
			static_cast<void>(entities_.visitEdgesUntil(
					snapshot, std::numeric_limits<size_t>::max(), [&](uint64_t edgeId, const EdgeEntityRecord &edge) {
					if (requested == edgeIds.edge_ids.size()) { // the callback stops after accepting the final requested Edge.
						return false;
					}
					const uint64_t wanted = edgeIds.edge_ids[requested];
					if (edgeId < wanted) {
						return true;
					}
					if (edgeId > wanted) { // the immutable Entity Map contains every published adjacency Edge.
						throwAdapterError(FormatError::FERR_INVALID_STATE,
										  "page-backed graph adjacency topology references a missing Edge");
					}
					const bool endpointMatches = direction == PageBackedGraphReadDirection::PBGRD_OUTGOING
														 ? edge.source_node_id == nodeId
														 : edge.target_node_id == nodeId;
						if (!endpointMatches) { // adjacency and Entity Map records share one pinned publication.
						throwAdapterError(FormatError::FERR_INVALID_STATE,
										  "page-backed graph adjacency topology has an inconsistent endpoint");
						}
						readSession.putEdge(edgeId, edge);
					if (!visitor(edgeId, edge)) {
						stoppedByVisitor = true;
						return false;
					}
					++requested;
					return requested < edgeIds.edge_ids.size();
				}));
		if (stoppedByVisitor) {
			return false;
		}
		if (requested != edgeIds.edge_ids.size()) { // the immutable Entity Map contains every published adjacency Edge.
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency topology Entity Map is missing an Edge");
		}
		return true;
	}

	bool PageBackedGraphReadAdapter::visitAdjacentEdgeRefsForNodesUntil(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const PageBackedGraphReadDirection direction, const size_t maximumRecords,
			const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session) const {
		if (nodeIds.empty() || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max() || !visitor ||
				!std::ranges::is_sorted(nodeIds) || std::ranges::adjacent_find(nodeIds) != nodeIds.end()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed batch adjacency traversal requires sorted nodes, a bound, and a visitor");
		}
		const GlobalAdjacencyDirection globalDirectionValue = globalDirection(direction);
		const size_t scanLimit = maximumRecords + 1U;
		NativeReadSession localSession;
		NativeReadSession &active = session == nullptr ? localSession : *session;
		active.bind(snapshot);
		std::vector<std::pair<uint64_t, uint64_t>> keys;
		keys.reserve(std::min(maximumRecords, MULTI_NODE_ADJACENCY_BATCH_ENTRIES));
		bool stoppedByVisitor = false;
		bool boundExceeded = false;
		size_t matchedRecords = 0U;
		const auto flush = [&]() {
			if (keys.empty()) {
				return true;
			}
			std::vector<uint64_t> edgeIds;
			edgeIds.reserve(keys.size());
			for (const auto &[nodeId, edgeId]: keys) {
				static_cast<void>(nodeId);
				edgeIds.push_back(edgeId);
			}
			std::ranges::sort(edgeIds);
			if (const auto duplicate = std::ranges::adjacent_find(edgeIds); duplicate != edgeIds.end()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
						"page-backed batch adjacency traversal returned a duplicate Edge");
			}
			const std::vector<std::optional<EdgeEntityRecord>> records = entities_.readEdgesSorted(snapshot, edgeIds);
			if (records.size() != edgeIds.size()) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
						"page-backed batch adjacency traversal changed Edge cardinality");
			}
			for (const auto &[nodeId, edgeId]: keys) {
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
					keys.emplace_back(nodeId, edgeId);
					return keys.size() != MULTI_NODE_ADJACENCY_BATCH_ENTRIES || flush();
				});
		if (stoppedByVisitor) {
			return false;
		}
		if (boundExceeded || !exhausted) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed batch adjacency traversal exceeded its Edge bound");
		}
		return flush();
	}

} // namespace graph::storage
