/**
 * @file IndexEntityManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "BaseEntityManager.hpp"
#include "graph/core/Index.hpp"

namespace graph::storage {

	class IndexEntityManager final : public BaseEntityManager<Index> {
	public:
		IndexEntityManager(const std::shared_ptr<DataManager> &dataManager,
						   std::shared_ptr<DeletionManager> deletionManager);

	protected:
		int64_t doAllocateId() override;

		void doRemove(Index &index) override;
	};

} // namespace graph::storage
