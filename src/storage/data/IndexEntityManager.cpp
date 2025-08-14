/**
 * @file IndexEntityManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/data/IndexEntityManager.hpp"
#include <utility>

namespace graph::storage {

	IndexEntityManager::IndexEntityManager(const std::shared_ptr<DataManager> &dataManager,
										   std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(deletionManager)) {}

	void IndexEntityManager::doRemove(Index &index) { deletionManager_->deleteIndex(index); }

	int64_t IndexEntityManager::doAllocateId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			throw std::runtime_error("DataManager is not available for ID allocation.");
		}
		return dataManager->getIdAllocator()->allocateId(Index::typeId);
	}

} // namespace graph::storage
