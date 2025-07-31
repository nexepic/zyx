/**
 * @file PropertyEntityManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/data/PropertyEntityManager.hpp"
#include <utility>

namespace graph::storage {

	PropertyEntityManager::PropertyEntityManager(const std::shared_ptr<DataManager> &dataManager,
												 std::shared_ptr<PropertyManager> propertyManager,
												 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(propertyManager), std::move(deletionManager)) {}

	void PropertyEntityManager::doRemove(Property &property) { deletionManager_->deleteProperty(property); }

	int64_t PropertyEntityManager::doAllocateId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			throw std::runtime_error("DataManager is not available for ID allocation.");
		}
		return dataManager->getIdAllocator()->allocateId(Property::typeId);
	}

} // namespace graph::storage
