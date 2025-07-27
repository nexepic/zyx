/**
 * @file PropertyEntityManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <utility>

#include "graph/storage/data/PropertyEntityManager.hpp"

namespace graph::storage {

	PropertyEntityManager::PropertyEntityManager(const std::shared_ptr<DataManager> &dataManager,
												 std::shared_ptr<PropertyManager> propertyManager,
												 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(propertyManager), std::move(deletionManager)) {}

	void PropertyEntityManager::doRemove(Property &property) { deletionManager_->deleteProperty(property); }

	int64_t PropertyEntityManager::doReserveTemporaryId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return 0;

		return dataManager->reserveTemporaryPropertyId();
	}

} // namespace graph::storage
