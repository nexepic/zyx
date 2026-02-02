/**
 * @file IndexEntityManager.cpp
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

#include "graph/storage/data/IndexEntityManager.hpp"
#include <utility>

namespace graph::storage {

	IndexEntityManager::IndexEntityManager(const std::shared_ptr<DataManager> &dataManager,
										   std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(deletionManager)) {}

	void IndexEntityManager::doRemove(Index &index) { deletionManager_->deleteIndex(index); }

	int64_t IndexEntityManager::doAllocateId() {
		return getDataManagerPtr()->getIdAllocator()->allocateId(Index::typeId);
	}

} // namespace graph::storage
