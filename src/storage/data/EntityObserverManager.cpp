/**
 * @file EntityObserverManager.cpp
 * @date 2026/3/31
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include "graph/storage/data/EntityObserverManager.hpp"

namespace graph::storage {

	void EntityObserverManager::registerObserver(std::shared_ptr<IEntityObserver> observer) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		observers_.push_back(std::move(observer));
	}

	void EntityObserverManager::registerValidator(std::shared_ptr<constraints::IEntityValidator> validator) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		validators_.push_back(std::move(validator));
	}

	void EntityObserverManager::notifyNodeAdded(const Node &node) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		for (const auto &observer : observers_) {
			observer->onNodeAdded(node);
		}
	}

	void EntityObserverManager::notifyNodesAdded(const std::vector<Node> &nodes) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		for (const auto &observer : observers_) {
			observer->onNodesAdded(nodes);
		}
	}

	void EntityObserverManager::notifyNodeUpdated(const Node &oldNode, const Node &newNode) const {
		if (suppressNotifications_) return;
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		for (const auto &observer : observers_) {
			observer->onNodeUpdated(oldNode, newNode);
		}
	}

	void EntityObserverManager::notifyNodeDeleted(const Node &node) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		for (const auto &observer : observers_) {
			observer->onNodeDeleted(node);
		}
	}

	void EntityObserverManager::notifyEdgeAdded(const Edge &edge) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		for (const auto &observer : observers_) {
			observer->onEdgeAdded(edge);
		}
	}

	void EntityObserverManager::notifyEdgesAdded(const std::vector<Edge> &edges) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		for (const auto &observer : observers_) {
			for (const auto &edge : edges) {
				observer->onEdgeAdded(edge);
			}
		}
	}

	void EntityObserverManager::notifyEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) const {
		if (suppressNotifications_) return;
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		for (const auto &observer : observers_) {
			observer->onEdgeUpdated(oldEdge, newEdge);
		}
	}

	void EntityObserverManager::notifyEdgeDeleted(const Edge &edge) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		for (const auto &observer : observers_) {
			observer->onEdgeDeleted(edge);
		}
	}

	void EntityObserverManager::notifyStateUpdated(const State &oldState, const State &newState) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		for (const auto &observer : observers_) {
			observer->onStateUpdated(oldState, newState);
		}
	}

} // namespace graph::storage
