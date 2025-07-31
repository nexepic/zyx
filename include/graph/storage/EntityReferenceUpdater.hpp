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

#include <memory>
#include "graph/storage/IDAllocator.hpp"

namespace graph::storage {

	class EntityReferenceUpdater {
	public:
		EntityReferenceUpdater(std::shared_ptr<std::fstream> file, std::shared_ptr<SegmentTracker> tracker);

		~EntityReferenceUpdater() = default;

		void updateEntityReferences(int64_t oldEntityId, const void *newEntity, uint32_t entityType);

	private:
		struct EntityPropertyInfo {
			PropertyStorageType storageType;
			int64_t propertyEntityId;
		};

		std::shared_ptr<std::fstream> file_;
		std::shared_ptr<SegmentTracker> segmentTracker_;

		void updateNodeReferences(int64_t oldNodeId, const Node &newNode);

		void updateEdgeReferences(int64_t oldEdgeId, const Edge &newEdge);

		void updatePropertyReferences(int64_t oldPropertyId, const Property &newProperty) const;

		void updateBlobReferences(int64_t oldBlobId, const Blob &newBlob) const;

		void updateBlobChainReference(int64_t oldBlobId, int64_t newBlobId, int64_t linkedBlobId, bool isNextBlob,
									  const Blob &sourceBlob) const;

		Property getPropertyById(int64_t propertyId) const;

		Blob getBlobById(int64_t blobId) const;

		template<typename T>
		static EntityPropertyInfo getEntityPropertyInfoFromEntity(const T &entity);

		template<typename T>
		void updatePropertiesPointingToEntity(int64_t oldEntityId, const T &newEntity);
	};

} // namespace graph::storage
