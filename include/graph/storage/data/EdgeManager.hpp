/**
 * @file EdgeManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "BaseEntityManager.hpp"
#include "graph/core/Edge.hpp"

namespace graph::storage {

	class EdgeManager : public BaseEntityManager<Edge> {
	public:
		EdgeManager(const std::shared_ptr<DataManager> &dataManager, std::shared_ptr<PropertyManager> propertyManager,
					std::shared_ptr<DeletionManager> deletionManager);

		// Override add to handle special edge creation logic (linking)
		void add(Edge &edge) override;

		// Edge-specific methods
		[[nodiscard]] std::vector<Edge> findByNode(int64_t nodeId, const std::string &direction = "both") const;

	protected:
		int64_t doAllocateId() override;

		void doRemove(Edge &edge) override;
	};

} // namespace graph::storage
