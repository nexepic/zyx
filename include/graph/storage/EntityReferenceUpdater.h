/**
 * @file EntityReferenceUpdater.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/5/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include "graph/storage/IDAllocator.h"

namespace graph::storage {

	class EntityReferenceUpdater {
	public:
		EntityReferenceUpdater(std::shared_ptr<std::fstream> file, std::shared_ptr<IDAllocator> idAllocator,
							   std::shared_ptr<SegmentTracker> tracker);

		~EntityReferenceUpdater() = default;

		/**
		 * Update temporary IDs to permanent IDs in Node references
		 * @param node The node to update
		 * @return True if any references were updated, false otherwise
		 */
		bool updateNodeReferencesToPermanent(Node &node);

		/**
		 * Update temporary IDs to permanent IDs in Edge references
		 * @param edge The edge to update
		 * @return True if any references were updated, false otherwise
		 */
		bool updateEdgeReferencesToPermanent(Edge &edge);

		/**
		 * Update temporary IDs to permanent IDs in Property references
		 * @param property The property to update
		 * @return True if any references were updated, false otherwise
		 */
		bool updatePropertyReferencesToPermanent(Property &property);

		/**
		 * Update temporary IDs to permanent IDs in Blob references, including chain links
		 * @param blob The blob to update
		 * @return True if any references were updated, false otherwise
		 */
		bool updateBlobReferencesToPermanent(Blob &blob);

		/**
		 * Update references to a specific entity (node, edge, property)
		 * @param entityId ID of the entity being updated
		 * @param entityType Type of the entity (Node, Edge, Property, Blob)
		 * @param oldSegmentOffset Original segment offset
		 * @param newSegmentOffset New segment offset
		 */
		void updateEntityReferences(int64_t entityId, uint32_t entityType, uint64_t oldSegmentOffset,
									uint64_t newSegmentOffset);

	private:
		std::shared_ptr<std::fstream> file_;
		std::shared_ptr<IDAllocator> idAllocator_;
		std::shared_ptr<SegmentTracker> segmentTracker_;

		void updateNodeReferences(int64_t oldNodeId, int64_t newNodeId, uint64_t oldOffset,
														  uint64_t newOffset);

		void updateEdgeReferences(int64_t oldEdgeId, int64_t newEdgeId, uint64_t oldOffset,
														  uint64_t newOffset);

		void updatePropertyReferences(int64_t oldPropId, int64_t newPropId, uint64_t oldOffset,
															  uint64_t newOffset);

		void updateBlobReferences(int64_t oldBlobId, int64_t newBlobId, uint64_t oldOffset, uint64_t newOffset);

		void updatePropertiesPointingToEntity(int64_t oldEntityId, int64_t newEntityId,
				      uint32_t entityType, uint64_t oldOffset, uint64_t newOffset);

		void updateEdgesPointingToNode(int64_t oldNodeId, int64_t newNodeId,
					       uint64_t oldOffset, uint64_t newOffset);

		uint64_t calculatePropertyPhysicalLocation(int64_t propId, uint64_t segmentOffset);

		int64_t getSegmentStartId(uint64_t segmentOffset, uint32_t segmentType);
	};

} // namespace graph::storage
