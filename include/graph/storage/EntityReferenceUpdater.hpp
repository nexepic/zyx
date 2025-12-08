/**
 * @file EntityReferenceUpdater.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/5/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <memory>
#include "graph/core/Types.hpp"

namespace graph {
    class Node;
    class Edge;
    class Property;
    class Blob;
    class Index;
    class State;
}

namespace graph::storage {

    class DataManager;

    /**
     * @brief Updates references to entities when their IDs change (e.g., during compaction).
     *
     * Refactored to use DataManager for all operations to ensure consistency with
     * the caching and transaction layers.
     */
    class EntityReferenceUpdater {
    public:
        explicit EntityReferenceUpdater(const std::shared_ptr<DataManager>& dataManager);

        void updateEntityReferences(int64_t oldEntityId, const void* newEntity, uint32_t entityType);

    private:
        std::weak_ptr<DataManager> dataManager_;

        // Helper structure
        struct EntityPropertyInfo {
            PropertyStorageType storageType;
            int64_t propertyEntityId;
        };

        // Specific updaters
        void updateNodeReferences(int64_t oldNodeId, const Node& newNode);
        void updateEdgeReferences(int64_t oldEdgeId, const Edge& newEdge);
        void updatePropertyReferences(int64_t oldPropId, const Property& newProperty) const;
        void updateBlobReferences(int64_t oldBlobId, const Blob& newBlob) const;
        void updateIndexReferences(int64_t oldIndexId, const Index& newIndex);
        void updateStateReferences(int64_t oldStateId, const State& newState);

        // Helper methods using DataManager
        template <typename T>
        static EntityPropertyInfo getEntityPropertyInfoFromEntity(const T& entity);

        template <typename T>
        void updatePropertiesPointingToEntity(int64_t oldEntityId, const T& newEntity);

        // Blob chain specific helper
        void updateBlobChainReference(int64_t oldBlobId, int64_t newBlobId,
                                      int64_t linkedBlobId, bool isNextBlob,
                                      const Blob& sourceBlob) const;

        // Index specific helpers
        void updateIndexParentPtr(int64_t childId, int64_t newParentId) const;
        void updateIndexSiblingPtr(int64_t siblingId, int64_t oldId, int64_t newId, bool updateNext) const;
        void updateIndexChildId(int64_t parentId, int64_t oldId, int64_t newId) const;

        // State specific helper
        void updateStateChainReference(int64_t targetStateId, int64_t oldId, int64_t newId, bool updateNext) const;
    };

} // namespace graph::storage