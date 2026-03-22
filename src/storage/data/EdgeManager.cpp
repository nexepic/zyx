/**
 * @file EdgeManager.cpp
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

#include "graph/storage/data/EdgeManager.hpp"
#include <utility>
#include "graph/traversal/RelationshipTraversal.hpp"

namespace graph::storage {

	EdgeManager::EdgeManager(DataManager* dataManager,
							 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(deletionManager)) {}

	void EdgeManager::doRemove(Edge &edge) {
		getDataManagerPtr()->getRelationshipTraversal()->unlinkEdge(edge);
		deletionManager_->deleteEdge(edge);
	}

	void EdgeManager::add(Edge &edge) {
		// Call base implementation
		BaseEntityManager::add(edge);

		// Edge-specific: Link the edge in the relationship traversal
		getDataManagerPtr()->getRelationshipTraversal()->linkEdge(edge);
	}

	std::vector<Edge> EdgeManager::findByNode(int64_t nodeId, const std::string &direction) const {
		return getDataManagerPtr()->findEdgesByNode(nodeId, direction);
	}

	int64_t EdgeManager::doAllocateId() {
		return getDataManagerPtr()->getIdAllocator()->allocateId(Edge::typeId);
	}

} // namespace graph::storage
