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

	/**
	 * Utility class to handle entity reference updates
	 * This class contains static methods to update references in different entity types
	 * without modifying the entity classes themselves.
	 */
	class EntityReferenceUpdater {
	public:
		/**
		 * Update temporary IDs to permanent IDs in Node references
		 * @param node The node to update
		 * @param idAllocator The ID allocator to resolve temporary to permanent IDs
		 * @return True if any references were updated, false otherwise
		 */
		static bool updateNodeReferencesToPermanent(Node &node, const std::shared_ptr<IDAllocator> &idAllocator);

		/**
		 * Update temporary IDs to permanent IDs in Edge references
		 * @param edge The edge to update
		 * @param idAllocator The ID allocator to resolve temporary to permanent IDs
		 * @return True if any references were updated, false otherwise
		 */
		static bool updateEdgeReferencesToPermanent(Edge &edge, const std::shared_ptr<IDAllocator> &idAllocator);

		/**
		 * Update temporary IDs to permanent IDs in Property references
		 * @param property The property to update
		 * @param idAllocator The ID allocator to resolve temporary to permanent IDs
		 * @return True if any references were updated, false otherwise
		 */
		static bool updatePropertyReferencesToPermanent(Property &property,
														const std::shared_ptr<IDAllocator> &idAllocator);

		/**
		 * Update temporary IDs to permanent IDs in Blob references, including chain links
		 * @param blob The blob to update
		 * @param idAllocator The ID allocator to resolve temporary to permanent IDs
		 * @return True if any references were updated, false otherwise
		 */
		static bool updateBlobReferencesToPermanent(Blob &blob, const std::shared_ptr<IDAllocator> &idAllocator);
	};

} // namespace graph::storage
