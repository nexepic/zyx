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
#include "graph/debug/PerfTrace.hpp"
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

	void EdgeManager::addBatch(std::vector<Edge> &edges) {
		auto linkUpdates = prepareAddBatch(edges);
		persistPreparedAddBatch(edges, linkUpdates);
	}

	traversal::RelationshipBatchLinkUpdates EdgeManager::prepareAddBatch(std::vector<Edge> &edges) {
		if (edges.empty()) {
			return {};
		}

		auto *dataManager = getDataManagerPtr();
		debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch.prepare_add");

		assignMissingIds(edges);

		auto traversal = dataManager->getRelationshipTraversal();
		return traversal->buildBatchLinks(edges);
	}

	void EdgeManager::persistPreparedAddBatch(
			const std::vector<Edge> &edges,
			const traversal::RelationshipBatchLinkUpdates &linkUpdates) {
		if (edges.empty()) {
			return;
		}

		auto *dataManager = getDataManagerPtr();
		debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch");
		{
			debug::ScopedPerfTimer upsertTimer("relationship_traversal.link_edges_batch.upsert_new_edges");
			persistAddedBatch(edges);
		}
		dataManager->getRelationshipTraversal()->applyBatchLinkUpdates(linkUpdates);
	}

	std::vector<Edge> EdgeManager::findByNode(int64_t nodeId, const std::string &direction) const {
		return getDataManagerPtr()->findEdgesByNode(nodeId, direction);
	}

	int64_t EdgeManager::doAllocateId() {
		return getDataManagerPtr()->getIdAllocator(EntityType::Edge)->allocate();
	}

} // namespace graph::storage
