#pragma once
#include <array>
#include <span>
#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::storage {
struct AdjacencyTopologyIdentity {
    uint64_t database_id=0,snapshot_generation=0,node_id=0,after_edge_id=0;
    LogicalGraphReadDirection direction=LogicalGraphReadDirection::LGRD_OUTGOING;
    uint64_t expected_first_edge_id=0; // exact logical continuation; zero for lower-bound/initial scan
    bool operator==(const AdjacencyTopologyIdentity &) const = default;
};
struct AdjacencyTopologyCandidate { uint64_t edge_id=0,neighbor_id=0,type_token=0; };
// Bounded verification boundary for future covering/segmented producers. Main
// Edge remains authoritative. Verify the COMPLETE private batch before any
// type filter or consumer side effect. Native producers must additionally bind
// the live pin/store provenance and authenticate their on-disk routes.
class VerifiedAdjacencyTopologyBatch final {
    std::array<EntityRef,64> entries_{};
    size_t count_=0;
    AdjacencyTopologyIdentity identity_;
    VerifiedAdjacencyTopologyBatch() = default;
public:
    [[nodiscard]] std::span<const EntityRef> entries() const & noexcept { return {entries_.data(),count_}; }
    [[nodiscard]] AdjacencyTopologyIdentity identity() const noexcept { return identity_; }
    std::span<const EntityRef> entries() const && = delete;
    template<class Lookup,class Checkpoint>
    [[nodiscard]] static VerifiedAdjacencyTopologyBatch verify(
        const AdjacencyTopologyIdentity &identity,const AdjacencyTopologyIdentity &expected,
        std::span<const AdjacencyTopologyCandidate> candidates,Lookup &&master,Checkpoint &&checkpoint) {
        if (!identity.database_id || !identity.snapshot_generation || !identity.node_id ||
            identity != expected || candidates.size()>64 ||
            (identity.direction!=LogicalGraphReadDirection::LGRD_OUTGOING &&
             identity.direction!=LogicalGraphReadDirection::LGRD_INCOMING))
            throw std::invalid_argument("adjacency batch identity, snapshot, or bound mismatch");
        if (identity.expected_first_edge_id &&
            (identity.expected_first_edge_id<=identity.after_edge_id || candidates.empty() ||
             candidates.front().edge_id!=identity.expected_first_edge_id))
            throw std::runtime_error("adjacency batch did not resume at its exact continuation");
        VerifiedAdjacencyTopologyBatch result;
        result.identity_=identity;
        uint64_t previous=identity.after_edge_id;
        for (const auto &candidate:candidates) {
            checkpoint();
            if (!candidate.edge_id || !candidate.neighbor_id || candidate.edge_id<=previous)
                throw std::runtime_error("adjacency batch has invalid or duplicate/nonmonotonic Edge identity");
            const EntityRef authoritative=master(candidate.edge_id);
            const bool outgoing=identity.direction==LogicalGraphReadDirection::LGRD_OUTGOING;
            if (!authoritative.is_edge || authoritative.entity_id!=candidate.edge_id ||
                !authoritative.source_node_id || !authoritative.target_node_id ||
                (outgoing ? authoritative.source_node_id : authoritative.target_node_id)!=identity.node_id ||
                (outgoing ? authoritative.target_node_id : authoritative.source_node_id)!=candidate.neighbor_id ||
                authoritative.type_token!=candidate.type_token)
                throw std::runtime_error("adjacency candidate disagrees with authoritative Edge");
            result.entries_[result.count_++]=authoritative;
            previous=candidate.edge_id;
        }
        checkpoint();
        return result;
    }
};
} // namespace graph::storage
