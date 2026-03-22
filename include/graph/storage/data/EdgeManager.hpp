/**
 * @file EdgeManager.hpp
 * @author Nexepic
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include "BaseEntityManager.hpp"
#include "graph/core/Edge.hpp"

namespace graph::storage {

	class EdgeManager final : public BaseEntityManager<Edge> {
	public:
		EdgeManager(DataManager* dataManager, std::shared_ptr<DeletionManager> deletionManager);

		// Override add to handle special edge creation logic (linking)
		void add(Edge &edge) override;

		// Edge-specific methods
		[[nodiscard]] std::vector<Edge> findByNode(int64_t nodeId, const std::string &direction = "both") const;

	protected:
		int64_t doAllocateId() override;

		void doRemove(Edge &edge) override;
	};

} // namespace graph::storage
