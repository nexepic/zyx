#pragma once
/**
 * @file RelocatableMetadataStore.cpp
 * @brief Serialized operations for the experimental relocatable Catalog metadata stack.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/property/HybridPropertyPoolRoutes.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/utils/OwnedVector.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"
#include "graph/storage/relocatable/metadata/MetadataStoreLifecycle.hpp"

#include <algorithm>
#include <exception>
#include <array>
#include <bit>
#include <limits>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/Sha256.hpp"

namespace graph::storage {

	namespace {

        bool isLargeValueMaintenancePool(const CatalogEntry &pool) {
            return (pool.kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL &&
                    pool.owner_id == RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID) ||
                (pool.kind == SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL &&
                    pool.owner_id == RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID) ||
                (pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL && pool.owner_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID);
        }

        using LogicalIdCapabilityMap = std::pmr::map<uint64_t, uint64_t>;
        struct PreparedLogicalIdCapabilities {
            LogicalIdCapabilityMap nodes;
            LogicalIdCapabilityMap edges;
            explicit PreparedLogicalIdCapabilities(std::pmr::memory_resource *memory) : nodes(memory), edges(memory) {}
        };
        // Prepare both kinds atomically. No allocating map insertion may follow
        // durable namespace publication or expose only half a returned lease.
        PreparedLogicalIdCapabilities prepareLogicalIdCapabilities(
                std::span<const NativeGraphLogicalIdLeaseRange> nodes,
                std::span<const NativeGraphLogicalIdLeaseRange> edges,
                const LogicalIdCapabilityMap &outstandingNodes,
                const LogicalIdCapabilityMap &outstandingEdges, const char *message) {
            PreparedLogicalIdCapabilities prepared(outstandingNodes.get_allocator().resource());
            const auto overlaps = [](const LogicalIdCapabilityMap &entries, uint64_t first, uint64_t last) {
                const auto next = entries.lower_bound(first);
                if (next != entries.end() && next->first <= last) return true;
                if (next == entries.begin()) return false;
                const auto previous = std::prev(next);
                return previous->second != 0U && previous->first + previous->second - 1U >= first;
            };
            const auto prepare = [&](std::span<const NativeGraphLogicalIdLeaseRange> ranges,
                                     const LogicalIdCapabilityMap &outstanding, LogicalIdCapabilityMap &target) {
                for (const auto &range : ranges) {
                    if (range.first_id == 0U || range.count == 0U ||
                        range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id)
                        throw FormatException(FormatError::FERR_INVALID_STATE, message);
                    const auto last = range.first_id + range.count - 1U;
                    if (overlaps(outstanding, range.first_id, last) || overlaps(target, range.first_id, last))
                        throw FormatException(FormatError::FERR_INVALID_STATE, message);
                    target.emplace(range.first_id, range.count);
                }
            };
            prepare(nodes, outstandingNodes, prepared.nodes);
            prepare(edges, outstandingEdges, prepared.edges);
            return prepared;
        }

		// This primitive only retires Page Directory routes. Its callers support the
		// four derived page-pool families whose owners need no other directory update.
		[[nodiscard]] bool matchesPageDirectoryOnlyPoolOwnerKind(SegmentKind poolKind, PageOwnerKind ownerKind) {
			switch (poolKind) {
				case SegmentKind::SGK_INDEX_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_INDEX;
				case SegmentKind::SGK_VECTOR_RAW_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_RAW;
				case SegmentKind::SGK_VECTOR_PQ_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_PQ;
				case SegmentKind::SGK_VECTOR_ADJACENCY_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_ADJACENCY;
				default: // validated B+Tree and Vector reclaimers construct retirement plans only for the four supported pool families above.
					return false;
			}
		}

        MetadataPagePoolAllocator openMetadataPoolWithControlScratch(
                const std::shared_ptr<StorageIO> &io, const Superblock &current,
                const std::shared_ptr<std::pmr::memory_resource> &controlOwner) {
            std::optional<utils::OwnedAllocationScope> scratch;
            if (controlOwner) scratch.emplace(controlOwner);
            // Guaranteed copy elision constructs the nonmovable allocator in
            // its final member; constructor recovery/discovery uses MAINT.
            return MetadataPagePoolAllocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY);
        }

	} // namespace


} // namespace graph::storage
