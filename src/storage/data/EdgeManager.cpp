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
							 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(deletionManager)) {}

	void EdgeManager::doRemove(Edge &edge) {
		if (const auto dataManager = dataManager_.lock()) {
			dataManager->getRelationshipTraversal()->unlinkEdge(edge);
			deletionManager_->deleteEdge(edge);
		}
	}

	void EdgeManager::add(Edge &edge) {
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

	int64_t EdgeManager::doAllocateId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			throw std::runtime_error("DataManager is not available for ID allocation.");
		}
		return dataManager->getIdAllocator()->allocateId(Edge::typeId);
	}

} // namespace graph::storage
