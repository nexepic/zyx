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

namespace graph::storage {

	IndexEntityManager::IndexEntityManager(std::shared_ptr<DataManager> dataManager,
							   std::shared_ptr<PropertyManager> propertyManager,
							   std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager<Index>(dataManager, propertyManager, deletionManager) {}

	void IndexEntityManager::doRemove(Index &index) { deletionManager_->deleteIndex(index); }

	int64_t IndexEntityManager::doReserveTemporaryId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return 0;

		return dataManager->reserveTemporaryIndexId();
	}

} // namespace graph::storage
