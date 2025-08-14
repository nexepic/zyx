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
#include <utility>

namespace graph::storage {

	NodeManager::NodeManager(const std::shared_ptr<DataManager> &dataManager,
							 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(deletionManager)) {}

	void NodeManager::doRemove(Node &node) { deletionManager_->deleteNode(node); }

	int64_t NodeManager::doAllocateId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			throw std::runtime_error("DataManager is not available for ID allocation.");
		}
		return dataManager->getIdAllocator()->allocateId(Node::typeId);
	}

} // namespace graph::storage
