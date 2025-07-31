/**
 * @file PropertyEntityManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "BaseEntityManager.hpp"
#include "graph/core/Property.hpp"

namespace graph::storage {

	class PropertyEntityManager : public BaseEntityManager<Property> {
	public:
		PropertyEntityManager(const std::shared_ptr<DataManager> &dataManager,
							  std::shared_ptr<PropertyManager> propertyManager,
							  std::shared_ptr<DeletionManager> deletionManager);

	protected:
		int64_t doAllocateId() override;

		void doRemove(Property &property) override;
	};

} // namespace graph::storage
