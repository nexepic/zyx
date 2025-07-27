/**
 * @file EdgeManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/data/EdgeManager.hpp"
#include <utility>
#include "graph/traversal/RelationshipTraversal.hpp"

namespace graph::storage {

	EdgeManager::EdgeManager(const std::shared_ptr<DataManager> &dataManager,
							 std::shared_ptr<PropertyManager> propertyManager,
							 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(propertyManager), std::move(deletionManager)) {}

	void EdgeManager::doRemove(Edge &edge) { deletionManager_->deleteEdge(edge); }

	void EdgeManager::add(const Edge &edge) {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		// Call base implementation
		BaseEntityManager::add(edge);

		// Edge-specific: Link the edge in the relationship traversal
		Edge edgeCopy = edge; // Make a mutable copy
		dataManager->getRelationshipTraversal()->linkEdge(edgeCopy);
	}

	std::vector<Edge> EdgeManager::findByNode(int64_t nodeId, const std::string &direction) const {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return {};

		return dataManager->findEdgesByNode(nodeId, direction);
	}

	int64_t EdgeManager::doReserveTemporaryId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return 0;

		return dataManager->reserveTemporaryEdgeId();
	}

} // namespace graph::storage
