/**
 * @file IndexEntityManager.hpp
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
