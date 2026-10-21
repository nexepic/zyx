/**
 * @file CoveredAdjacencyCandidate.hpp
 * @brief Bounded native covering-tree laboratory protocol, without adoption.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <limits>
#include "graph/storage/AdjacencyTopologyContract.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/index/FixedBTreeMap.hpp"

namespace graph::storage {
// No reserved namespace or automatic writer hook. The native coordinator must
// maintain both directions and main Edge in one graph transaction. Main Edge
// authentication stays mandatory; global completeness requires integrity work.
class CoveredAdjacencyCandidate final {
public:
    using Map = FixedBTreeMap<RSS_GLOBAL_ADJACENCY_KEY_BYTES, 16>;
    struct Page {
        VerifiedAdjacencyTopologyBatch topology;
        std::optional<AdjacencyTopologyIdentity> continuation;
        size_t candidate_records = 0; // includes the authenticated lookahead
    };
    struct IntegrityReport {
        DatabaseId database_id{};
        Generation generation{};
        size_t edges = 0, candidate_entries = 0, edge_metadata_pages = 0;
    };
    [[nodiscard]] static Map::Key key(GlobalAdjacencyDirection direction, uint64_t node, uint64_t edge) {
        if (!edge) throw FormatException(FormatError::FERR_INVALID_HEADER, "covered candidate requires a positive EdgeId");
        auto result = AdjacencyKeyCodec::lowerBoundFixed(direction, node);
        for (unsigned i = 0; i < 8; ++i) result[9 + i] = std::byte((edge >> (56U - i * 8U)) & 255U);
        return result;
    }
    [[nodiscard]] static Map::Entry entry(GlobalAdjacencyDirection direction, uint64_t edge,
        uint64_t source, uint64_t target, uint64_t type) {
        if (!source || !target) throw FormatException(FormatError::FERR_INVALID_HEADER, "covered candidate has a zero endpoint");
        Map::Value value{};
        LittleEndianCodec::writeU64(value, 0, direction == GlobalAdjacencyDirection::GAD_OUT ? target : source);
        LittleEndianCodec::writeU64(value, 8, type);
        return {key(direction, direction == GlobalAdjacencyDirection::GAD_OUT ? source : target, edge), value};
    }
    [[nodiscard]] static Page scan(BTreePageStore &pages, EntityPageStore &entities,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot, uint64_t indexId,
        GlobalAdjacencyDirection direction, uint64_t node, size_t maximumRecords,
        std::optional<AdjacencyTopologyIdentity> continuation = {},
        const std::function<void(size_t)> &boundaryAdmission = {}, const CancellationCheckpoint *cancellation = nullptr) {
        if (!metadata.ownsSnapshot(snapshot) || !pages.usesMetadataStore(metadata) || !entities.usesMetadataStore(metadata) ||
            !indexId || (indexId >> 63U) || !node || !maximumRecords || maximumRecords > 64U ||
            (direction != GlobalAdjacencyDirection::GAD_OUT && direction != GlobalAdjacencyDirection::GAD_IN))
            throw FormatException(FormatError::FERR_INVALID_HEADER, "covered candidate requires its domain, namespace, direction and bound");
        const auto logicalDirection = direction == GlobalAdjacencyDirection::GAD_OUT ?
            LogicalGraphReadDirection::LGRD_OUTGOING : LogicalGraphReadDirection::LGRD_INCOMING;
        AdjacencyTopologyIdentity identity{snapshot.superblock().database_id.value(), snapshot.superblock().generation.value(),
            node, 0, logicalDirection, 0};
        if (continuation) {
            identity.after_edge_id = continuation->after_edge_id;
            identity.expected_first_edge_id = continuation->expected_first_edge_id;
            if (identity != *continuation || !identity.after_edge_id || identity.expected_first_edge_id <= identity.after_edge_id)
                throw FormatException(FormatError::FERR_INVALID_HEADER, "covered candidate continuation has foreign or invalid identity");
        }
        const auto check = [&] { if (cancellation) cancellation->check(); };
        check(); static_cast<void>(entities.readNode(snapshot, node));
        const auto root = metadata.findIndexRootOptional(snapshot, indexId);
        if (!root) throw FormatException(FormatError::FERR_INVALID_STATE, "covered candidate namespace is not installed");
        Map map(indexId, pages, metadata, BTreeRootReadBinding{snapshot.superblock().database_id,
            snapshot.superblock().generation, *root});
        std::array<AdjacencyTopologyCandidate, 65> candidates{}; size_t count = 0;
        const auto first = key(direction, node, identity.after_edge_id + 1U);
        static_cast<void>(map.visitPrefixFrom(snapshot, std::span(first).first(9), first, maximumRecords + 1U,
            [&](const Map::Entry &record) {
                check(); const auto decoded = AdjacencyKeyCodec::decode(record.key);
                candidates[count++] = {decoded.edge_id, LittleEndianCodec::readU64(record.value, 0),
                    LittleEndianCodec::readU64(record.value, 8)};
                return true;
            }, boundaryAdmission, cancellation));
        const auto master = [&](uint64_t edge) {
            const auto record = entities.readEdge(snapshot, edge);
            return EntityRef{edge, true, record.source_node_id, record.target_node_id, record.type_id};
        };
        const size_t returned = (std::min)(count, maximumRecords);
        auto verified = VerifiedAdjacencyTopologyBatch::verify(identity, identity,
            std::span(candidates).first(returned), master, check);
        std::optional<AdjacencyTopologyIdentity> next;
        if (count > returned) {
            auto lookahead = identity;
            lookahead.after_edge_id = candidates[returned - 1U].edge_id;
            lookahead.expected_first_edge_id = candidates[returned].edge_id;
            static_cast<void>(VerifiedAdjacencyTopologyBatch::verify(lookahead, lookahead,
                std::span(candidates).subspan(returned, 1), master, check));
            next = lookahead;
        }
        check(); return {std::move(verified), next, count};
    }
    // Explicit global reconciliation, never a point-read or planning fallback.
    // The report is returned only after BOTH scans exhaust under their bounds.
    // It describes this pin; it does not grant cache/adoption/write authority.
    [[nodiscard]] static IntegrityReport verifyIntegrity(BTreePageStore &pages, EntityPageStore &entities,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot, uint64_t indexId,
        size_t maximumEdges, size_t maximumEdgeMetadataPages,
        const std::function<void(size_t)> &boundaryAdmission = {}, const CancellationCheckpoint *cancellation = nullptr) {
        const auto require = [](bool valid, const char *message) {
            if (!valid) throw FormatException(FormatError::FERR_INVALID_STATE, message);
        };
        require(metadata.ownsSnapshot(snapshot) && pages.usesMetadataStore(metadata) && entities.usesMetadataStore(metadata) &&
            indexId && !(indexId >> 63U) && maximumEdges && maximumEdgeMetadataPages &&
            maximumEdges <= ((std::numeric_limits<size_t>::max)() - 2U) / 2U,
            "covered candidate integrity requires its domain, namespace and finite independent bounds");
        const auto check = [&] {
            require(metadata.ownsSnapshot(snapshot), "covered candidate integrity lost its source pin");
            if (cancellation) cancellation->check();
        };
        check();
        IntegrityReport report{snapshot.superblock().database_id, snapshot.superblock().generation};
        const auto root = metadata.findIndexRootOptional(snapshot, indexId);
        std::optional<Map> map;
        if (root) map.emplace(indexId, pages, metadata,
            BTreeRootReadBinding{report.database_id, report.generation, *root});
        if (map) {
            const bool exhausted = map->visitAll(snapshot, 2U * maximumEdges + 1U,
                [&](const Map::Entry &record) {
                    check();
                    if (++report.candidate_entries > 2U * maximumEdges)
                        throw std::length_error("covered candidate integrity exceeds its entry bound");
                    const auto decoded = AdjacencyKeyCodec::decode(record.key);
                    const auto edge = entities.readEdge(snapshot, decoded.edge_id);
                    const auto expected = entry(decoded.direction, decoded.edge_id, edge.source_node_id, edge.target_node_id, edge.type_id);
                    require(record == expected, "covered candidate entry disagrees with its authoritative Edge");
                    return true;
                }, boundaryAdmission, cancellation);
            require(exhausted, "covered candidate integrity has an incomplete candidate scan");
        }
        const bool exhausted = metadata.visitEdgePageLocationsAfter(snapshot, 0, maximumEdgeMetadataPages,
            [&](const EntityPageLocation &location) {
                check();
                if (++report.edges > maximumEdges)
                    throw std::length_error("covered candidate integrity exceeds its authoritative Edge bound");
                require(bool(map), "covered candidate has live Edges without an installed tree");
                const auto edge = entities.readEdgeAt(snapshot, location);
                static_cast<void>(entities.readNode(snapshot, edge.source_node_id));
                if (edge.target_node_id != edge.source_node_id) static_cast<void>(entities.readNode(snapshot, edge.target_node_id));
                for (const auto direction : {GlobalAdjacencyDirection::GAD_OUT, GlobalAdjacencyDirection::GAD_IN}) {
                    const auto expected = entry(direction, location.entity_id, edge.source_node_id, edge.target_node_id, edge.type_id);
                    require(map->find(snapshot, expected.key, boundaryAdmission, cancellation) == expected.value,
                        "covered candidate is missing an exact directional Edge entry");
                }
                return true;
            }, &report.edge_metadata_pages);
        require(exhausted && report.candidate_entries == 2U * report.edges,
            "covered candidate integrity has an incomplete Edge scan or inconsistent global cardinality");
        check(); return report;
    }
};
} // namespace graph::storage
