/**
 * @file NodeManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "BaseEntityManager.hpp"
#include "graph/core/Node.hpp"

namespace graph::storage {

	class DataManager;
	class PropertyManager;

	/**
	 * Manages node entities
	 */
	class NodeManager : public BaseEntityManager<Node> {
	public:
		NodeManager(const std::shared_ptr<DataManager>& dataManager, std::shared_ptr<PropertyManager> propertyManager,
					std::shared_ptr<DeletionManager> deletionManager);

	protected:
		int64_t doAllocateId() override;

		void doRemove(Node &node) override;
	};

} // namespace graph::storage
