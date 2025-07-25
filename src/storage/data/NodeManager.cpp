/**
 * @file NodeManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/data/NodeManager.hpp"

namespace graph::storage {

	NodeManager::NodeManager(std::shared_ptr<DataManager> dataManager, std::shared_ptr<PropertyManager> propertyManager,
							 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, propertyManager, deletionManager) {}

	void NodeManager::doRemove(Node &node) { deletionManager_->deleteNode(node); }

	int64_t NodeManager::doReserveTemporaryId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return 0;

		return dataManager->reserveTemporaryNodeId();
	}

} // namespace graph::storage
